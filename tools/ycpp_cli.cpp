// ycpp — command line tool for Yjs-compatible text documents.
//
// A document is stored on disk as a plain Yjs v1 update (the same bytes
// y.encodeStateAsUpdate(doc) produces in JavaScript), so doc files can be
// exchanged directly with any Yjs peer.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "base64_io.hpp"
#include "ycpp/ycpp.hpp"

namespace {

void usage() {
  std::cerr <<
      "usage: ycpp <command> [args]\n"
      "\n"
      "  new    <doc>                  create an empty document file\n"
      "  show   <doc>                  print the document text\n"
      "  insert <doc> <index> <text>   insert text at UTF-16 index\n"
      "  delete <doc> <index> <len>    delete len UTF-16 units at index\n"
      "  export <doc>                  print full state as base64 update\n"
      "  apply  <doc> [base64]         apply a base64 update (arg or stdin)\n"
      "  sv     <doc>                  print base64 state vector\n"
      "  diff   <doc> <base64-sv>      print base64 update relative to a state vector\n"
      "\n"
      "Doc files are raw Yjs v1 updates; the text lives in the root type \"default\".\n"
      "Set YCPP_CLIENT_ID to fix the client id (useful for tests).\n";
}

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open " + path);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::vector<uint8_t>& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) throw std::runtime_error("cannot write " + path);
  f.write(reinterpret_cast<const char*>(data.data()),
          static_cast<std::streamsize>(data.size()));
}

std::unique_ptr<ycpp::Doc> makeDoc() {
  std::optional<uint64_t> clientId;
  if (const char* env = std::getenv("YCPP_CLIENT_ID")) {
    clientId = std::stoull(env);
  }
  return std::make_unique<ycpp::Doc>(clientId);
}

std::unique_ptr<ycpp::Doc> loadDoc(const std::string& path) {
  auto doc = makeDoc();
  auto bytes = readFile(path);
  if (!bytes.empty()) doc->applyUpdate(bytes);
  return doc;
}

void saveDoc(const std::string& path, const ycpp::Doc& doc) {
  writeFile(path, doc.encodeStateAsUpdate());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage();
    return 2;
  }
  std::string cmd = argv[1];
  std::string path = argv[2];
  try {
    if (cmd == "new") {
      ycpp::Doc doc;
      saveDoc(path, doc);
    } else if (cmd == "show") {
      auto doc = loadDoc(path);
      std::cout << doc->getText().toString() << "\n";
    } else if (cmd == "insert") {
      if (argc < 5) { usage(); return 2; }
      auto doc = loadDoc(path);
      doc->getText().insert(std::stoull(argv[3]), argv[4]);
      saveDoc(path, *doc);
    } else if (cmd == "delete") {
      if (argc < 5) { usage(); return 2; }
      auto doc = loadDoc(path);
      doc->getText().remove(std::stoull(argv[3]), std::stoull(argv[4]));
      saveDoc(path, *doc);
    } else if (cmd == "export") {
      auto doc = loadDoc(path);
      std::cout << ycpp_tools::base64Encode(doc->encodeStateAsUpdate()) << "\n";
    } else if (cmd == "apply") {
      auto doc = loadDoc(path);
      std::string b64;
      if (argc >= 4) {
        b64 = argv[3];
      } else {
        std::stringstream ss;
        ss << std::cin.rdbuf();
        b64 = ss.str();
      }
      doc->applyUpdate(ycpp_tools::base64Decode(b64));
      saveDoc(path, *doc);
      std::cout << doc->getText().toString() << "\n";
    } else if (cmd == "sv") {
      auto doc = loadDoc(path);
      std::cout << ycpp_tools::base64Encode(doc->encodeStateVector()) << "\n";
    } else if (cmd == "diff") {
      if (argc < 4) { usage(); return 2; }
      auto doc = loadDoc(path);
      std::cout << ycpp_tools::base64Encode(
                       doc->encodeStateAsUpdate(ycpp_tools::base64Decode(argv[3])))
                << "\n";
    } else {
      usage();
      return 2;
    }
  } catch (const std::exception& e) {
    std::cerr << "ycpp: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
