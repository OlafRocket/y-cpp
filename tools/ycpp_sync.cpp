// ycpp-sync — live synchronization tool for y-cpp, built on IXWebSocket.
//
//   ycpp-sync serve   [--port N] [--host H] [--web DIR] [--doc FILE] [--headless]
//   ycpp-sync connect <ws://host:port> [--doc FILE] [--headless]
//
// serve:   hosts a ycpp document behind an HTTP + WebSocket server (same port).
//          GET / serves web/live.html (real Yjs peers in the browser); every
//          WebSocket peer receives the full state on connect, and every binary
//          message is a Yjs v1 update that is applied and relayed to the others.
// connect: joins a server as a native peer with its own ycpp document.
//
// In both modes, when stdin is a terminal, a minimal terminal text box lets
// you edit the shared text directly from the console (POSIX terminals; on
// Windows use --headless and edit through the browser page).
//
// All CRDT logic lives in the ycpp library; this tool only moves bytes.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include "ycpp/utf.hpp"
#include "ycpp/ycpp.hpp"

#ifndef _WIN32
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

std::atomic<bool> gQuit{false};
void onSignal(int) { gQuit = true; }

std::vector<uint8_t> toBytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}
std::string toPayload(const std::vector<uint8_t>& v) {
  return std::string(v.begin(), v.end());
}

// ------------------------------------------------------------- shared doc

struct SharedDoc {
  std::mutex mu;
  ycpp::Doc doc;
  std::string docFile;

  void load() {
    if (docFile.empty()) return;
    std::ifstream f(docFile, std::ios::binary);
    if (!f) return;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    if (!bytes.empty()) doc.applyUpdate(bytes);
  }

  void saveLocked() {  // call with mu held
    if (docFile.empty()) return;
    auto bytes = doc.encodeStateAsUpdate();
    std::ofstream f(docFile, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  }
};

// --------------------------------------------------------- terminal text box

#ifndef _WIN32

bool isHighSurrogate(char16_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool isLowSurrogate(char16_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

class RawTerm {
 public:
  bool enable() {
    if (!isatty(STDIN_FILENO)) return false;
    if (tcgetattr(STDIN_FILENO, &orig_) != 0) return false;
    termios raw = orig_;
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    enabled_ = true;
    writeOut("\x1b[?1049h");  // alternate screen
    return true;
  }
  ~RawTerm() { disable(); }
  void disable() {
    if (!enabled_) return;
    writeOut("\x1b[?1049l");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_);
    enabled_ = false;
  }
  static void writeOut(const std::string& s) {
    ssize_t r = write(STDOUT_FILENO, s.data(), s.size());
    (void)r;
  }

 private:
  termios orig_{};
  bool enabled_ = false;
};

// A minimal text box over the shared document. Local edits are encoded as
// state-vector diffs and handed to `send`; remote changes are applied by the
// network callbacks, which use adjustCursorLocked() + markDirty().
class Editor {
 public:
  Editor(SharedDoc& sd, std::function<void(const std::vector<uint8_t>&)> send,
         std::string title, std::function<std::string()> statusFn)
      : sd_(sd), send_(std::move(send)), title_(std::move(title)),
        statusFn_(std::move(statusFn)) {}

  void markDirty() { dirty_ = true; }

  // Keep the cursor near its logical position across a remote change.
  // Call with sd_.mu held.
  void adjustCursorLocked(const std::u16string& before, const std::u16string& after) {
    uint64_t p = 0;
    while (p < before.size() && p < after.size() && before[p] == after[p]) p++;
    uint64_t sfx = 0;
    while (sfx < before.size() - p && sfx < after.size() - p &&
           before[before.size() - 1 - sfx] == after[after.size() - 1 - sfx]) {
      sfx++;
    }
    if (cursor_ <= p) {
      // unchanged region before the cursor: keep position
    } else if (cursor_ >= before.size() - sfx) {
      cursor_ = after.size() - (before.size() - cursor_);
    } else {
      cursor_ = std::min<uint64_t>(p, after.size());
    }
  }

  void run() {
    RawTerm term;
    if (!term.enable()) {
      std::cerr << "ycpp-sync: stdin is not a terminal, running headless\n";
      runHeadlessWait();
      return;
    }
    std::string pending;
    markDirty();
    while (!gQuit) {
      if (dirty_.exchange(false)) render();
      pollfd pf{STDIN_FILENO, POLLIN, 0};
      int r = poll(&pf, 1, 100);
      if (r <= 0) continue;
      char buf[512];
      ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
      if (n <= 0) continue;
      pending.append(buf, static_cast<size_t>(n));
      processInput(pending);
      markDirty();
    }
  }

  static void runHeadlessWait() {
    while (!gQuit) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

 private:
  // ------------------------------------------------------------ edit ops

  void insertText(const std::u16string& s) {
    std::vector<uint8_t> diff;
    {
      std::lock_guard<std::mutex> lk(sd_.mu);
      auto t = sd_.doc.getText();
      if (cursor_ > t.length()) cursor_ = t.length();
      auto sv = sd_.doc.encodeStateVector();
      t.insertUtf16(cursor_, s);
      cursor_ += s.size();
      diff = sd_.doc.encodeStateAsUpdate(sv);
      sd_.saveLocked();
    }
    send_(diff);
  }

  void removeRange(uint64_t from, uint64_t to) {  // call with mu NOT held
    std::vector<uint8_t> diff;
    {
      std::lock_guard<std::mutex> lk(sd_.mu);
      auto t = sd_.doc.getText();
      if (to > t.length() || from >= to) return;
      auto sv = sd_.doc.encodeStateVector();
      t.remove(from, to - from);
      cursor_ = from;
      diff = sd_.doc.encodeStateAsUpdate(sv);
      sd_.saveLocked();
    }
    send_(diff);
  }

  static uint64_t stepLeft(const std::u16string& t, uint64_t c) {
    if (c == 0) return 0;
    if (c >= 2 && isLowSurrogate(t[c - 1]) && isHighSurrogate(t[c - 2])) return c - 2;
    return c - 1;
  }
  static uint64_t stepRight(const std::u16string& t, uint64_t c) {
    if (c >= t.size()) return t.size();
    if (c + 1 < t.size() && isHighSurrogate(t[c]) && isLowSurrogate(t[c + 1])) return c + 2;
    return c + 1;
  }

  std::u16string snapshot() {
    std::lock_guard<std::mutex> lk(sd_.mu);
    auto u = sd_.doc.getText().toUtf16();
    if (cursor_ > u.size()) cursor_ = u.size();
    return u;
  }

  void backspace() {
    auto u = snapshot();
    uint64_t nc = stepLeft(u, cursor_);
    if (nc < cursor_) removeRange(nc, cursor_);
  }
  void del() {
    auto u = snapshot();
    uint64_t nr = stepRight(u, cursor_);
    if (nr > cursor_) removeRange(cursor_, nr);
  }
  void moveLeft() {
    auto u = snapshot();
    std::lock_guard<std::mutex> lk(sd_.mu);
    cursor_ = stepLeft(u, cursor_);
  }
  void moveRight() {
    auto u = snapshot();
    std::lock_guard<std::mutex> lk(sd_.mu);
    cursor_ = stepRight(u, cursor_);
  }
  static void lineBounds(const std::u16string& u, uint64_t pos, uint64_t& start,
                         uint64_t& end) {
    start = pos;
    while (start > 0 && u[start - 1] != u'\n') start--;
    end = pos;
    while (end < u.size() && u[end] != u'\n') end++;
  }
  void fixSurrogateLanding(const std::u16string& u) {
    if (cursor_ > 0 && cursor_ < u.size() && isLowSurrogate(u[cursor_]) &&
        isHighSurrogate(u[cursor_ - 1])) {
      cursor_--;
    }
  }
  void moveUp() {
    auto u = snapshot();
    std::lock_guard<std::mutex> lk(sd_.mu);
    uint64_t ls, le;
    lineBounds(u, cursor_, ls, le);
    if (ls == 0) { cursor_ = 0; return; }
    uint64_t col = cursor_ - ls;
    uint64_t pls, ple;
    lineBounds(u, ls - 1, pls, ple);
    cursor_ = pls + std::min(col, ple - pls);
    fixSurrogateLanding(u);
  }
  void moveDown() {
    auto u = snapshot();
    std::lock_guard<std::mutex> lk(sd_.mu);
    uint64_t ls, le;
    lineBounds(u, cursor_, ls, le);
    if (le >= u.size()) { cursor_ = u.size(); return; }
    uint64_t col = cursor_ - ls;
    uint64_t ns, ne;
    lineBounds(u, le + 1, ns, ne);
    cursor_ = ns + std::min(col, ne - ns);
    fixSurrogateLanding(u);
  }
  void home() {
    auto u = snapshot();
    std::lock_guard<std::mutex> lk(sd_.mu);
    uint64_t ls, le;
    lineBounds(u, cursor_, ls, le);
    cursor_ = ls;
  }
  void end() {
    auto u = snapshot();
    std::lock_guard<std::mutex> lk(sd_.mu);
    uint64_t ls, le;
    lineBounds(u, cursor_, ls, le);
    cursor_ = le;
  }

  // ------------------------------------------------------------- input

  void processInput(std::string& pending) {
    while (!pending.empty() && !gQuit) {
      unsigned char c = static_cast<unsigned char>(pending[0]);
      if (c == 0x1b) {  // escape sequence
        if (pending.size() == 1) return;  // wait for the rest
        if (pending[1] != '[' && pending[1] != 'O') {
          pending.erase(0, 1);
          continue;
        }
        size_t i = 2;
        while (i < pending.size() && (pending[i] < 0x40 || pending[i] > 0x7e)) i++;
        if (i >= pending.size()) return;  // incomplete
        char fin = pending[i];
        std::string param = pending.substr(2, i - 2);
        pending.erase(0, i + 1);
        if (fin == 'D') moveLeft();
        else if (fin == 'C') moveRight();
        else if (fin == 'A') moveUp();
        else if (fin == 'B') moveDown();
        else if (fin == 'H') home();
        else if (fin == 'F') end();
        else if (fin == '~') {
          if (param == "3") del();
          else if (param == "1" || param == "7") home();
          else if (param == "4" || param == "8") end();
        }
        continue;
      }
      if (c == 0x03 || c == 0x11) { gQuit = true; return; }  // Ctrl-C / Ctrl-Q
      if (c == 0x0c) { pending.erase(0, 1); continue; }      // Ctrl-L: redraw
      if (c == '\r' || c == '\n') {
        insertText(u"\n");
        pending.erase(0, 1);
        continue;
      }
      if (c == 0x7f || c == 0x08) {
        backspace();
        pending.erase(0, 1);
        continue;
      }
      if (c < 0x20) { pending.erase(0, 1); continue; }
      // Printable text (possibly a paste): consume a maximal run of complete
      // UTF-8 sequences and insert them in one operation.
      size_t j = 0;
      while (j < pending.size()) {
        unsigned char b = static_cast<unsigned char>(pending[j]);
        if (b < 0x20 || b == 0x7f) break;
        size_t l = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3
                 : (b >> 3) == 0x1E ? 4 : 1;
        if (j + l > pending.size()) break;  // incomplete sequence at the end
        j += l;
      }
      if (j == 0) return;  // wait for the rest of a UTF-8 sequence
      insertText(ycpp::utf::utf8To16(pending.substr(0, j)));
      pending.erase(0, j);
    }
  }

  // ------------------------------------------------------------- render

  void render() {
    std::u16string u16;
    uint64_t cursor;
    {
      std::lock_guard<std::mutex> lk(sd_.mu);
      u16 = sd_.doc.getText().toUtf16();
      if (cursor_ > u16.size()) cursor_ = u16.size();
      cursor = cursor_;
    }
    winsize wsz{};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsz);
    uint64_t rows = wsz.ws_row > 4 ? wsz.ws_row : 24;
    uint64_t cols = wsz.ws_col > 10 ? wsz.ws_col : 80;

    // Split into lines and find the cursor's row/column.
    std::vector<std::u16string> lines(1);
    uint64_t curRow = 0, curCol = 0;
    for (uint64_t i = 0; i < u16.size(); i++) {
      if (i == cursor) { curRow = lines.size() - 1; curCol = lines.back().size(); }
      if (u16[i] == u'\n') lines.emplace_back();
      else lines.back().push_back(u16[i]);
    }
    if (cursor == u16.size()) { curRow = lines.size() - 1; curCol = lines.back().size(); }

    uint64_t visible = rows - 3;
    if (curRow < top_) top_ = curRow;
    if (curRow >= top_ + visible) top_ = curRow - visible + 1;

    std::string out = "\x1b[H\x1b[2J";
    out += "\x1b[1m" + title_ + "\x1b[0m  " + statusFn_() +
           "  \x1b[2m[Ctrl-C quits]\x1b[0m\r\n";
    out += std::string(static_cast<size_t>(cols), '-') + "\r\n";
    for (uint64_t r = top_; r < lines.size() && r < top_ + visible; r++) {
      std::u16string line = lines[r];
      if (line.size() > cols - 1) line.resize(cols - 1);
      out += ycpp::utf::utf16To8(line) + "\r\n";
    }
    uint64_t screenRow = curRow - top_ + 3;
    uint64_t screenCol = std::min<uint64_t>(curCol, cols - 1) + 1;
    out += "\x1b[" + std::to_string(screenRow) + ";" + std::to_string(screenCol) + "H";
    RawTerm::writeOut(out);
  }

  SharedDoc& sd_;
  std::function<void(const std::vector<uint8_t>&)> send_;
  std::string title_;
  std::function<std::string()> statusFn_;
  uint64_t cursor_ = 0;  // UTF-16 units, guarded by sd_.mu
  uint64_t top_ = 0;     // first visible row
  std::atomic<bool> dirty_{true};
};

#else  // _WIN32: networking works, the terminal text box is not ported

class Editor {
 public:
  Editor(SharedDoc&, std::function<void(const std::vector<uint8_t>&)>,
         std::string, std::function<std::string()>) {}
  void markDirty() {}
  void adjustCursorLocked(const std::u16string&, const std::u16string&) {}
  void run() { runHeadlessWait(); }
  static void runHeadlessWait() {
    while (!gQuit) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
};

#endif

// ---------------------------------------------------------------- http

std::string contentTypeFor(const std::string& path) {
  auto ends = [&](const char* s) {
    size_t n = strlen(s);
    return path.size() >= n && path.compare(path.size() - n, n, s) == 0;
  };
  if (ends(".html")) return "text/html; charset=utf-8";
  if (ends(".js")) return "application/javascript";
  if (ends(".css")) return "text/css";
  if (ends(".ico")) return "image/x-icon";
  return "application/octet-stream";
}

std::string findWebDir(const std::string& fromArgs) {
  if (!fromArgs.empty()) return fromArgs;
  std::ifstream local("web/live.html");
  if (local) return "web";
#ifdef YCPP_WEB_DIR
  return YCPP_WEB_DIR;
#else
  return "web";
#endif
}

// ---------------------------------------------------------------- modes

struct Options {
  int port = 8765;
  std::string host = "0.0.0.0";
  std::string web;
  std::string doc;
  bool headless = false;
  std::string url;  // connect mode
};

int runServe(const Options& opts) {
  SharedDoc sd;
  sd.docFile = opts.doc;
  sd.load();

  std::string webDir = findWebDir(opts.web);
  ix::HttpServer server(opts.port, opts.host);

  std::unique_ptr<Editor> editor;

  // WebSocket side: full state on connect, then relay updates.
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,
                                        ix::WebSocket& ws,
                                        const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) {
      std::string payload;
      {
        std::lock_guard<std::mutex> lk(sd.mu);
        payload = toPayload(sd.doc.encodeStateAsUpdate());
      }
      ws.sendBinary(payload);
      if (editor) editor->markDirty();
      if (opts.headless) std::cout << "peer connected" << std::endl;
    } else if (msg->type == ix::WebSocketMessageType::Message && msg->binary) {
      {
        std::lock_guard<std::mutex> lk(sd.mu);
        std::u16string before = sd.doc.getText().toUtf16();
        try {
          sd.doc.applyUpdate(toBytes(msg->str));
        } catch (const std::exception& e) {
          std::cerr << "ycpp-sync: bad update ignored: " << e.what() << std::endl;
          return;
        }
        if (editor) editor->adjustCursorLocked(before, sd.doc.getText().toUtf16());
        sd.saveLocked();
      }
      for (const auto& client : server.getClients()) {
        if (client.get() != &ws) client->sendBinary(msg->str);
      }
      if (editor) editor->markDirty();
    } else if (msg->type == ix::WebSocketMessageType::Close) {
      if (editor) editor->markDirty();
      if (opts.headless) std::cout << "peer disconnected" << std::endl;
    }
  });

  // HTTP side: serve the live test page.
  server.setOnConnectionCallback(
      [webDir](ix::HttpRequestPtr req, std::shared_ptr<ix::ConnectionState>)
          -> ix::HttpResponsePtr {
        std::string path = req->uri.substr(0, req->uri.find('?'));
        if (path == "/") path = "/live.html";
        if (path.find("..") != std::string::npos) {
          return std::make_shared<ix::HttpResponse>(400, "Bad Request");
        }
        std::ifstream f(webDir + path, std::ios::binary);
        if (!f) {
          return std::make_shared<ix::HttpResponse>(
              404, "Not Found", ix::HttpErrorCode::Ok,
              ix::WebSocketHttpHeaders{{"Content-Type", "text/plain"}},
              "not found");
        }
        std::stringstream body;
        body << f.rdbuf();
        return std::make_shared<ix::HttpResponse>(
            200, "OK", ix::HttpErrorCode::Ok,
            ix::WebSocketHttpHeaders{{"Content-Type", contentTypeFor(path)}},
            body.str());
      });

  auto res = server.listen();
  if (!res.first) {
    std::cerr << "ycpp-sync: listen failed: " << res.second << std::endl;
    return 1;
  }
  server.start();
  std::cout << "ycpp-sync: listening on http://localhost:" << opts.port
            << " (WebSocket on the same port, web dir: " << webDir << ")"
            << std::endl;

  auto broadcastAll = [&server](const std::vector<uint8_t>& diff) {
    std::string payload = toPayload(diff);
    for (const auto& client : server.getClients()) client->sendBinary(payload);
  };
  auto status = [&server, &sd]() {
    size_t peers = server.getClients().size();
    std::lock_guard<std::mutex> lk(sd.mu);
    return std::to_string(peers) + " peer(s), " +
           std::to_string(sd.doc.getText().length()) + " chars";
  };

  if (opts.headless) {
    Editor::runHeadlessWait();
  } else {
    editor = std::make_unique<Editor>(
        sd, broadcastAll, "ycpp-sync serve :" + std::to_string(opts.port), status);
    editor->run();
    editor.reset();
  }
  server.stop();
  {
    std::lock_guard<std::mutex> lk(sd.mu);
    sd.saveLocked();
  }
  return 0;
}

int runConnect(const Options& opts) {
  SharedDoc sd;
  sd.docFile = opts.doc;
  sd.load();

  ix::WebSocket ws;
  ws.setUrl(opts.url);

  std::unique_ptr<Editor> editor;
  std::atomic<bool> connected{false};

  ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) {
      connected = true;
      std::string payload;
      {
        std::lock_guard<std::mutex> lk(sd.mu);
        payload = toPayload(sd.doc.encodeStateAsUpdate());
      }
      ws.sendBinary(payload);  // offer our state; server replies with its own
      if (editor) editor->markDirty();
    } else if (msg->type == ix::WebSocketMessageType::Message && msg->binary) {
      std::lock_guard<std::mutex> lk(sd.mu);
      std::u16string before = sd.doc.getText().toUtf16();
      try {
        sd.doc.applyUpdate(toBytes(msg->str));
      } catch (const std::exception& e) {
        std::cerr << "ycpp-sync: bad update ignored: " << e.what() << std::endl;
        return;
      }
      if (editor) editor->adjustCursorLocked(before, sd.doc.getText().toUtf16());
      sd.saveLocked();
      if (editor) editor->markDirty();
    } else if (msg->type == ix::WebSocketMessageType::Close ||
               msg->type == ix::WebSocketMessageType::Error) {
      connected = false;
      if (editor) editor->markDirty();
    }
  });
  ws.start();  // reconnects automatically

  auto send = [&ws](const std::vector<uint8_t>& diff) {
    ws.sendBinary(toPayload(diff));
  };
  auto status = [&connected, &sd]() {
    std::string s = connected ? "connected" : "connecting...";
    std::lock_guard<std::mutex> lk(sd.mu);
    return s + ", " + std::to_string(sd.doc.getText().length()) + " chars";
  };

  if (opts.headless) {
    std::cout << "ycpp-sync: connecting to " << opts.url << std::endl;
    Editor::runHeadlessWait();
  } else {
    editor = std::make_unique<Editor>(sd, send, "ycpp-sync " + opts.url, status);
    editor->run();
    editor.reset();
  }
  ws.stop();
  {
    std::lock_guard<std::mutex> lk(sd.mu);
    sd.saveLocked();
  }
  return 0;
}

void usage() {
  std::cerr <<
      "usage:\n"
      "  ycpp-sync serve   [--port N] [--host H] [--web DIR] [--doc FILE] [--headless]\n"
      "  ycpp-sync connect <ws://host:port> [--doc FILE] [--headless]\n"
      "\n"
      "serve   hosts the shared text: browsers open http://host:port/ (live Yjs\n"
      "        page), native peers use `ycpp-sync connect`, all edits sync live.\n"
      "connect joins a server as a native peer.\n"
      "Without --headless (and with a TTY) a minimal terminal text box opens.\n"
      "--doc FILE persists the document as a Yjs v1 update file.\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  Options opts;
  std::string mode = argv[1];
  for (int i = 2; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) { usage(); std::exit(2); }
      return argv[++i];
    };
    if (a == "--port") opts.port = std::stoi(next());
    else if (a == "--host") opts.host = next();
    else if (a == "--web") opts.web = next();
    else if (a == "--doc") opts.doc = next();
    else if (a == "--headless") opts.headless = true;
    else if (a.rfind("ws://", 0) == 0 || a.rfind("wss://", 0) == 0) opts.url = a;
    else { usage(); return 2; }
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif
  ix::initNetSystem();

  int rc = 2;
  if (mode == "serve") {
    rc = runServe(opts);
  } else if (mode == "connect" && !opts.url.empty()) {
    rc = runConnect(opts);
  } else {
    usage();
  }
  ix::uninitNetSystem();
  return rc;
}
