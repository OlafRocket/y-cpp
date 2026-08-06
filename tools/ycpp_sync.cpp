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
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <ixwebsocket/IXHttpServer.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include "ycpp/protocols/awareness.hpp"
#include "ycpp/protocols/sync.hpp"
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

std::span<const uint8_t> asBytes(const std::string& s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}
std::string toPayload(const std::vector<uint8_t>& v) {
  return std::string(v.begin(), v.end());
}

// ------------------------------------------------------------- shared doc

// y-websocket message envelope (one varUint in front of every message).
constexpr uint64_t kMessageSync = 0;
constexpr uint64_t kMessageAwareness = 1;
constexpr uint64_t kMessageAuth = 2;
constexpr uint64_t kMessageQueryAwareness = 3;

struct SharedDoc {
  std::mutex mu;
  ycpp::Doc doc;
  ycpp::protocols::Awareness awareness{doc.clientId()};
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

// A minimal text box over the shared document. Local edits run in doc
// transactions; the doc's onUpdate handler (registered by serve/connect)
// broadcasts them. Remote changes are applied by the network callbacks,
// which use adjustCursorLocked() + markDirty().
class Editor {
 public:
  Editor(SharedDoc& sd, std::string title, std::function<std::string()> statusFn)
      : sd_(sd), title_(std::move(title)), statusFn_(std::move(statusFn)) {}

  void markDirty() { dirty_ = true; }

  // Cursor preservation across remote changes, via relative positions:
  // capture before applying a remote update, restore after. Call with
  // sd_.mu held.
  ycpp::RelativePosition captureCursorLocked() {
    return ycpp::createRelativePosition(sd_.doc.getText(), cursor_);
  }
  void restoreCursorLocked(const ycpp::RelativePosition& rel) {
    if (auto abs = ycpp::resolveRelativePosition(rel, sd_.doc)) {
      cursor_ = abs->index;
    } else {
      cursor_ = std::min<uint64_t>(cursor_, sd_.doc.getText().length());
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
    std::lock_guard<std::mutex> lk(sd_.mu);
    auto t = sd_.doc.getText();
    if (cursor_ > t.length()) cursor_ = t.length();
    t.insertUtf16(cursor_, s);  // onUpdate handler broadcasts the transaction
    cursor_ += s.size();
    sd_.saveLocked();
  }

  void removeRange(uint64_t from, uint64_t to) {
    std::lock_guard<std::mutex> lk(sd_.mu);
    auto t = sd_.doc.getText();
    if (to > t.length() || from >= to) return;
    t.remove(from, to - from);
    cursor_ = from;
    sd_.saveLocked();
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
  std::string title_;
  std::function<std::string()> statusFn_;
  uint64_t cursor_ = 0;  // UTF-16 units, guarded by sd_.mu
  uint64_t top_ = 0;     // first visible row
  std::atomic<bool> dirty_{true};
};

#else  // _WIN32: networking works, the terminal text box is not ported

class Editor {
 public:
  Editor(SharedDoc&, std::string, std::function<std::string()>) {}
  void markDirty() {}
  ycpp::RelativePosition captureCursorLocked() { return {}; }
  void restoreCursorLocked(const ycpp::RelativePosition&) {}
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
  // A y-websocket server exposes no state of its own.
  sd.awareness.setLocalState(std::nullopt);

  std::string webDir = findWebDir(opts.web);
  ix::HttpServer server(opts.port, opts.host);

  std::unique_ptr<Editor> editor;
  // Which awareness client ids arrived through which connection (so a closed
  // connection removes exactly its clients). Guarded by sd.mu.
  std::map<std::string, std::set<uint64_t>> connClients;

  auto sendTo = [](ix::WebSocket& ws, const ycpp::lib0::Encoder& enc) {
    ws.sendBinary(toPayload(enc.bytes()));
  };
  auto broadcast = [&server](const ycpp::lib0::Encoder& enc) {
    std::string payload = toPayload(enc.bytes());
    for (const auto& client : server.getClients()) client->sendBinary(payload);
  };

  // Every document transaction goes out as a sync Update message (local edits
  // and applied remote updates alike, as the y-websocket server does).
  sd.doc.onUpdate([&](const std::vector<uint8_t>& update, const std::string&) {
    ycpp::lib0::Encoder enc;
    enc.writeVarUint(kMessageSync);
    ycpp::protocols::sync::writeUpdate(enc, update);
    broadcast(enc);
  });

  // Awareness changes fan out to everyone; track per-connection client ids.
  sd.awareness.onUpdate([&](const ycpp::protocols::Awareness::Change& c) {
    std::vector<uint64_t> changed;
    changed.insert(changed.end(), c.added.begin(), c.added.end());
    changed.insert(changed.end(), c.updated.begin(), c.updated.end());
    changed.insert(changed.end(), c.removed.begin(), c.removed.end());
    if (c.origin.rfind("conn:", 0) == 0) {
      auto& owned = connClients[c.origin];
      for (uint64_t cl : c.added) owned.insert(cl);
      for (uint64_t cl : c.updated) owned.insert(cl);
      for (uint64_t cl : c.removed) owned.erase(cl);
    }
    ycpp::lib0::Encoder enc;
    enc.writeVarUint(kMessageAwareness);
    enc.writeVarUint8Array(sd.awareness.encodeUpdate(changed));
    broadcast(enc);
  });

  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState> state,
                                        ix::WebSocket& ws,
                                        const ix::WebSocketMessagePtr& msg) {
    std::string connId = "conn:" + state->getId();
    if (msg->type == ix::WebSocketMessageType::Open) {
      std::lock_guard<std::mutex> lk(sd.mu);
      ycpp::lib0::Encoder enc;
      enc.writeVarUint(kMessageSync);
      ycpp::protocols::sync::writeSyncStep1(enc, sd.doc);
      sendTo(ws, enc);
      if (!sd.awareness.getStates().empty()) {
        ycpp::lib0::Encoder aenc;
        aenc.writeVarUint(kMessageAwareness);
        aenc.writeVarUint8Array(sd.awareness.encodeAllStates());
        sendTo(ws, aenc);
      }
      if (editor) editor->markDirty();
      if (opts.headless) std::cout << "peer connected" << std::endl;
    } else if (msg->type == ix::WebSocketMessageType::Message && msg->binary) {
      std::lock_guard<std::mutex> lk(sd.mu);
      try {
        ycpp::lib0::Decoder dec(asBytes(msg->str));
        switch (dec.readVarUint()) {
          case kMessageSync: {
            ycpp::RelativePosition cursor;
            if (editor) cursor = editor->captureCursorLocked();
            ycpp::lib0::Encoder reply;
            reply.writeVarUint(kMessageSync);
            ycpp::protocols::sync::readSyncMessage(dec, reply, sd.doc, "remote");
            if (editor) editor->restoreCursorLocked(cursor);
            if (reply.bytes().size() > 1) sendTo(ws, reply);
            sd.saveLocked();
            break;
          }
          case kMessageAwareness:
            sd.awareness.applyUpdate(dec.readVarUint8Array(), connId);
            break;
          case kMessageQueryAwareness: {
            ycpp::lib0::Encoder aenc;
            aenc.writeVarUint(kMessageAwareness);
            aenc.writeVarUint8Array(sd.awareness.encodeAllStates());
            sendTo(ws, aenc);
            break;
          }
          case kMessageAuth:
            break;  // accepted, ignored
          default:
            break;
        }
      } catch (const std::exception& e) {
        std::cerr << "ycpp-sync: bad message ignored: " << e.what() << std::endl;
      }
      if (editor) editor->markDirty();
    } else if (msg->type == ix::WebSocketMessageType::Close) {
      std::lock_guard<std::mutex> lk(sd.mu);
      auto it = connClients.find(connId);
      if (it != connClients.end()) {
        std::vector<uint64_t> gone(it->second.begin(), it->second.end());
        connClients.erase(it);
        if (!gone.empty()) sd.awareness.removeStates(gone, "disconnect");
      }
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
          // y-websocket providers append the room name to the URL path;
          // any unknown path serves the page (single-room server).
          std::ifstream page(webDir + "/live.html", std::ios::binary);
          if (!page) {
            return std::make_shared<ix::HttpResponse>(
                404, "Not Found", ix::HttpErrorCode::Ok,
                ix::WebSocketHttpHeaders{{"Content-Type", "text/plain"}}, "not found");
          }
          std::stringstream body;
          body << page.rdbuf();
          return std::make_shared<ix::HttpResponse>(
              200, "OK", ix::HttpErrorCode::Ok,
              ix::WebSocketHttpHeaders{{"Content-Type", "text/html; charset=utf-8"}},
              body.str());
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
            << " (y-websocket protocol, web dir: " << webDir << ")" << std::endl;

  // Awareness housekeeping (y-protocols: 30s timeout).
  std::thread housekeeper([&] {
    int ticks = 0;
    while (!gQuit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (++ticks % 6 != 0) continue;  // every 3s
      std::lock_guard<std::mutex> lk(sd.mu);
      sd.awareness.removeOutdated();
    }
  });

  auto status = [&server, &sd]() {
    size_t peers = server.getClients().size();
    std::lock_guard<std::mutex> lk(sd.mu);
    return std::to_string(peers) + " conn(s), " +
           std::to_string(sd.awareness.getStates().size()) + " present, " +
           std::to_string(sd.doc.getText().length()) + " chars";
  };

  if (opts.headless) {
    Editor::runHeadlessWait();
  } else {
    editor = std::make_unique<Editor>(
        sd, "ycpp-sync serve :" + std::to_string(opts.port), status);
    editor->run();
    editor.reset();
  }
  gQuit = true;
  housekeeper.join();
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
  sd.awareness.setLocalStateField(
      "user", ycpp::Any::object({{"name", ycpp::Any::of("ycpp")},
                                 {"color", ycpp::Any::of("#6ea8ff")}}));

  ix::WebSocket ws;
  ws.setUrl(opts.url);

  std::unique_ptr<Editor> editor;
  std::atomic<bool> connected{false};

  auto send = [&ws](const ycpp::lib0::Encoder& enc) {
    ws.sendBinary(toPayload(enc.bytes()));
  };

  // Local document transactions go to the server as Update messages.
  sd.doc.onUpdate([&](const std::vector<uint8_t>& update, const std::string& origin) {
    if (origin == "remote") return;
    ycpp::lib0::Encoder enc;
    enc.writeVarUint(kMessageSync);
    ycpp::protocols::sync::writeUpdate(enc, update);
    send(enc);
  });
  // Local awareness changes (incl. periodic renewal) are broadcast.
  sd.awareness.onUpdate([&](const ycpp::protocols::Awareness::Change& c) {
    if (c.origin != "local") return;
    ycpp::lib0::Encoder enc;
    enc.writeVarUint(kMessageAwareness);
    enc.writeVarUint8Array(sd.awareness.encodeUpdate({sd.awareness.clientId()}));
    send(enc);
  });

  ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) {
      connected = true;
      std::lock_guard<std::mutex> lk(sd.mu);
      ycpp::lib0::Encoder enc;
      enc.writeVarUint(kMessageSync);
      ycpp::protocols::sync::writeSyncStep1(enc, sd.doc);
      send(enc);
      ycpp::lib0::Encoder q;
      q.writeVarUint(kMessageQueryAwareness);
      send(q);
      ycpp::lib0::Encoder aenc;
      aenc.writeVarUint(kMessageAwareness);
      aenc.writeVarUint8Array(sd.awareness.encodeUpdate({sd.awareness.clientId()}));
      send(aenc);
      if (editor) editor->markDirty();
    } else if (msg->type == ix::WebSocketMessageType::Message && msg->binary) {
      std::lock_guard<std::mutex> lk(sd.mu);
      try {
        ycpp::lib0::Decoder dec(asBytes(msg->str));
        switch (dec.readVarUint()) {
          case kMessageSync: {
            ycpp::RelativePosition cursor;
            if (editor) cursor = editor->captureCursorLocked();
            ycpp::lib0::Encoder reply;
            reply.writeVarUint(kMessageSync);
            ycpp::protocols::sync::readSyncMessage(dec, reply, sd.doc, "remote");
            if (editor) editor->restoreCursorLocked(cursor);
            if (reply.bytes().size() > 1) send(reply);
            sd.saveLocked();
            break;
          }
          case kMessageAwareness:
            sd.awareness.applyUpdate(dec.readVarUint8Array(), "remote");
            break;
          case kMessageQueryAwareness: {
            ycpp::lib0::Encoder aenc;
            aenc.writeVarUint(kMessageAwareness);
            aenc.writeVarUint8Array(sd.awareness.encodeUpdate({sd.awareness.clientId()}));
            send(aenc);
            break;
          }
          default:
            break;
        }
      } catch (const std::exception& e) {
        std::cerr << "ycpp-sync: bad message ignored: " << e.what() << std::endl;
      }
      if (editor) editor->markDirty();
    } else if (msg->type == ix::WebSocketMessageType::Close ||
               msg->type == ix::WebSocketMessageType::Error) {
      connected = false;
      if (editor) editor->markDirty();
    }
  });
  ws.start();  // reconnects automatically

  // Keep-alive: renew the local awareness state and drop stale peers.
  std::thread housekeeper([&] {
    int ticks = 0;
    while (!gQuit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      ticks++;
      std::lock_guard<std::mutex> lk(sd.mu);
      if (ticks % 6 == 0) sd.awareness.removeOutdated();
      if (ticks % 30 == 0) sd.awareness.setLocalState(sd.awareness.getLocalState());
    }
  });

  auto status = [&connected, &sd]() {
    std::string s = connected ? "connected" : "connecting...";
    std::lock_guard<std::mutex> lk(sd.mu);
    return s + ", " + std::to_string(sd.awareness.getStates().size()) + " present, " +
           std::to_string(sd.doc.getText().length()) + " chars";
  };

  if (opts.headless) {
    std::cout << "ycpp-sync: connecting to " << opts.url << std::endl;
    Editor::runHeadlessWait();
  } else {
    editor = std::make_unique<Editor>(sd, "ycpp-sync " + opts.url, status);
    editor->run();
    editor.reset();
  }
  gQuit = true;
  housekeeper.join();
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
