// y-cpp: minimal C++ implementation of the Yjs CRDT protocol (v1 encoding),
// supporting collaborative plain text (Y.Text with root types only).
//
// Ported from the reference implementation yjs v13 (https://github.com/yjs/yjs);
// struct/field names intentionally mirror the yjs sources so the two can be
// read side by side.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ycpp/lib0.hpp"

namespace ycpp {

struct ID {
  uint64_t client = 0;
  uint64_t clock = 0;
  bool operator==(const ID& o) const { return client == o.client && clock == o.clock; }
};

// Content refs of the Yjs v1 encoding. The minimal build supports the subset
// needed for plain text; other refs are rejected at decode time.
enum class ContentKind : uint8_t {
  GC = 0,       // deleted range from a peer that garbage-collected (no content)
  Deleted = 1,  // ContentDeleted: tombstone with length only
  String = 4,   // ContentString: run of text, length in UTF-16 code units
};

class Doc;
struct Text;

struct Item {
  ID id;
  uint64_t len = 0;
  std::optional<ID> origin;       // ID of item to the left at insertion time
  std::optional<ID> rightOrigin;  // ID of item to the right at insertion time
  Item* left = nullptr;
  Item* right = nullptr;
  Text* parent = nullptr;
  std::optional<std::string> parentName;  // root type key ("ykey")
  std::optional<ID> parentId;             // parent addressed by item ID (unsupported)
  std::optional<std::string> parentSub;   // map key (unsupported)
  ContentKind kind = ContentKind::String;
  std::u16string str;  // content for ContentKind::String
  bool deleted = false;
  bool integrated = false;

  ID lastId() const { return ID{id.client, id.clock + len - 1}; }
  bool countable() const { return kind == ContentKind::String; }
};

// A collaborative text type (Y.Text without formatting attributes).
// Indices and lengths are in UTF-16 code units, matching JavaScript strings.
struct Text {
  std::string name;
  Item* start = nullptr;
  uint64_t length = 0;
  Doc* doc = nullptr;

  void insert(uint64_t index, const std::string& utf8);
  void insertUtf16(uint64_t index, const std::u16string& s);
  void remove(uint64_t index, uint64_t delLen);
  std::string toString() const;      // UTF-8
  std::u16string toUtf16() const;
};

struct DeleteRange {
  uint64_t clock = 0;
  uint64_t len = 0;
};

class Doc {
 public:
  explicit Doc(std::optional<uint64_t> clientId = std::nullopt);
  ~Doc();
  Doc(const Doc&) = delete;
  Doc& operator=(const Doc&) = delete;

  uint64_t clientId() const { return clientId_; }
  Text& getText(const std::string& name = "default");

  // Apply a Yjs v1 update. Structs with missing causal dependencies are kept
  // pending and retried on subsequent calls.
  void applyUpdate(const std::vector<uint8_t>& update);

  std::vector<uint8_t> encodeStateAsUpdate() const;
  // remoteStateVector: output of encodeStateVector() on the remote doc.
  std::vector<uint8_t> encodeStateAsUpdate(const std::vector<uint8_t>& remoteStateVector) const;
  std::vector<uint8_t> encodeStateVector() const;

  // Next expected clock for a client (0 if unknown).
  uint64_t getState(uint64_t client) const;

  // True if some received structs/deletes still await missing dependencies.
  bool hasPending() const { return !pending_.empty() || !pendingDs_.empty(); }

 private:
  friend struct Text;

  Item* alloc();
  void addStruct(Item* it);
  Item* getItem(const ID& id) const;
  Item* getItemCleanStart(const ID& id);
  Item* getItemCleanEnd(const ID& id);
  Item* splitItem(Item* it, uint64_t diff);
  void markDeleted(Item* s);

  bool depsMissing(const Item* it) const;
  void integrate(Item* it, uint64_t offset);
  void integrateAsGc(Item* it, uint64_t offset);
  void integrateLoop();
  // Applies one delete range; returns the not-yet-applicable remainder (len 0 if none).
  DeleteRange applyDeleteRange(uint64_t client, uint64_t clock, uint64_t len);

  // One encoded struct-section entry: an item (with offset) or a Skip gap.
  struct Emit {
    const Item* it = nullptr;  // nullptr → Skip of skipLen
    uint64_t offset = 0;
    uint64_t skipLen = 0;
  };
  bool buildClientEmits(uint64_t client, uint64_t fromClock, std::vector<Emit>& out,
                        uint64_t& startClock) const;
  void writeStruct(lib0::Encoder& enc, const Item* it, uint64_t offset) const;
  void writeDeleteSet(lib0::Encoder& enc) const;
  std::vector<uint8_t> encodeStateAsUpdateSv(const std::map<uint64_t, uint64_t>& sv) const;

  uint64_t clientId_;
  std::map<uint64_t, std::vector<Item*>> store_;          // per client, sorted by clock
  std::map<std::string, std::unique_ptr<Text>> types_;    // root types
  std::vector<std::unique_ptr<Item>> items_;              // ownership of all items
  std::map<uint64_t, std::deque<Item*>> pending_;         // structs awaiting deps
  std::map<uint64_t, std::vector<DeleteRange>> pendingDs_;  // deletes awaiting structs
};

}  // namespace ycpp
