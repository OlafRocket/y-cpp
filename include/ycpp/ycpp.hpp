// y-cpp: minimal C++ implementation of the Yjs CRDT protocol (v1 encoding),
// supporting collaborative plain text (Y.Text), sequences (Y.Array), and
// key-value maps (Y.Map) on root and nested types.
//
// Ported from the reference implementation yjs v13 (https://github.com/yjs/yjs);
// struct/field names intentionally mirror the yjs sources so the two can be
// read side by side.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "ycpp/any.hpp"
#include "ycpp/lib0.hpp"

namespace ycpp {

struct ID {
  uint64_t client = 0;
  uint64_t clock = 0;
  bool operator==(const ID& o) const { return client == o.client && clock == o.clock; }
};

// Content refs of the Yjs v1 encoding. Text/Any/Binary/Type carry the data
// model; JSON/Embed/Format/Doc are decoded, preserved, and re-encoded
// opaquely (no dedicated API); everything occupies clock space.
enum class ContentKind : uint8_t {
  GC = 0,       // deleted range from a peer that garbage-collected (no content)
  Deleted = 1,  // ContentDeleted: tombstone with length only
  JSON = 2,     // legacy ContentJSON (raw JSON strings, opaque)
  Binary = 3,   // ContentBinary: one Uint8Array value
  String = 4,   // ContentString: run of text, length in UTF-16 code units
  Embed = 5,    // ContentEmbed (raw JSON, opaque)
  Format = 6,   // ContentFormat (rich-text attribute, opaque, not countable)
  Type = 7,     // ContentType: a nested shared type
  Any = 8,      // ContentAny: run of lib0 "Any" values
  Doc = 9,      // ContentDoc (subdocument reference, opaque)
};

// Wire ids of shared-type kinds (yjs writeTypeRef).
enum class TypeKind : uint8_t {
  Array = 0, Map = 1, Text = 2,
  XmlElement = 3, XmlFragment = 4, XmlHook = 5, XmlText = 6,
  Unknown = 255,  // root referenced by an update before any local getX() call
};

class Doc;
struct YType;

// The result of reading one position/key: either a plain value or a nested
// shared type (inspect with the wrapper classes below).
struct Value {
  Any any;
  YType* type = nullptr;
  bool isType() const { return type != nullptr; }
};

// One operation of a list-type change description (Quill-style delta).
struct DeltaOp {
  enum class Kind { Retain, Insert, Delete };
  Kind kind = Kind::Retain;
  uint64_t length = 0;        // retain/delete count; also insert length
  std::u16string str;         // inserted text (from ContentString items)
  std::vector<Value> values;  // inserted values (from other content)
};

// One changed key of a map-type change description.
struct MapChange {
  enum class Action { Add, Update, Delete };
  Action action = Action::Add;
  std::optional<Value> oldValue;  // value before the transaction (Update/Delete)
};

// Fired on observers at the end of the transaction that changed the type.
// Computed like yjs YEvent.changes/.keys: from the transaction's before-state
// and delete set, before any content is garbage-collected.
struct Event {
  YType* target = nullptr;
  std::string origin;  // transact()/applyUpdate() origin tag
  bool local = true;   // false when produced by applyUpdate
  std::vector<DeltaOp> delta;         // list changes (Text/Array)
  std::map<std::string, MapChange> keys;  // map key changes
};

using Observer = std::function<void(const Event&)>;

struct Item {
  ID id;
  uint64_t len = 0;
  std::optional<ID> origin;       // ID of item to the left at insertion time
  std::optional<ID> rightOrigin;  // ID of item to the right at insertion time
  Item* left = nullptr;
  Item* right = nullptr;
  YType* parent = nullptr;
  std::optional<std::string> parentName;  // root type key ("ykey"), from decode
  std::optional<ID> parentId;             // parent addressed by item ID, from decode
  std::optional<std::string> parentSub;   // map key (Y.Map entries)
  ContentKind kind = ContentKind::String;
  std::u16string str;          // String content
  std::vector<Any> anyVals;    // Any content; also JSON/Embed/Doc raw payloads
  std::vector<uint8_t> bin;    // Binary content
  std::string key;             // Format attribute key / Doc guid
  YType* typeContent = nullptr;  // Type content
  bool deleted = false;
  bool integrated = false;

  ID lastId() const { return ID{id.client, id.clock + len - 1}; }
  bool countable() const {
    switch (kind) {
      case ContentKind::String: case ContentKind::Any: case ContentKind::Binary:
      case ContentKind::Type: case ContentKind::JSON: case ContentKind::Embed:
      case ContentKind::Doc:
        return true;
      default:
        return false;
    }
  }
};

// A shared type node (yjs AbstractType): a linked list of items (_start) for
// sequence content and a key -> item map (_map) for map content.
struct YType {
  TypeKind kind = TypeKind::Unknown;
  std::string rootName;                // set for root types
  std::string xmlName;                 // node/hook name for XmlElement/XmlHook
  Item* item = nullptr;                // the ContentType item (null for roots)
  Item* start = nullptr;
  std::map<std::string, Item*> map;    // parentSub -> most recent item
  uint64_t length = 0;                 // countable list length (UTF-16 for text)
  Doc* doc = nullptr;
  std::vector<std::pair<uint64_t, Observer>> observers;
};

class Text;
class Array;
class Map;

// RAII observer registration: detaches automatically when destroyed (like
// boost::signals2::scoped_connection). Movable, not copyable. Must not
// outlive the Doc it was created from.
class Subscription {
 public:
  Subscription() = default;
  Subscription(const Subscription&) = delete;
  Subscription& operator=(const Subscription&) = delete;
  Subscription(Subscription&& o) noexcept { *this = std::move(o); }
  Subscription& operator=(Subscription&& o) noexcept {
    if (this != &o) {
      reset();
      type_ = o.type_;
      doc_ = o.doc_;
      handle_ = o.handle_;
      o.type_ = nullptr;
      o.doc_ = nullptr;
      o.handle_ = 0;
    }
    return *this;
  }
  ~Subscription() { reset(); }

  void reset();  // detach now (idempotent)
  bool active() const { return handle_ != 0; }

 private:
  friend class TypeRef;
  friend class Doc;
  YType* type_ = nullptr;  // set for type observers
  Doc* doc_ = nullptr;     // set for doc update handlers
  uint64_t handle_ = 0;
};

// ------------------------------------------------------------------ wrappers

class TypeRef {
 public:
  TypeRef(YType* t) : t_(t) {}
  uint64_t length() const { return t_->length; }
  YType* node() const { return t_; }

  // Observers fire at the end of the transaction that changed this type
  // (local edits and applyUpdate alike). Returns a handle for unobserve().
  uint64_t observe(Observer fn);
  void unobserve(uint64_t handle);

  // RAII variant of observe(): the observer detaches when the returned
  // Subscription is destroyed or reset().
  Subscription subscribe(Observer fn);

 protected:
  YType* t_;
};

// Y.Text: indices and lengths in UTF-16 code units, matching JavaScript.
class Text : public TypeRef {
 public:
  using TypeRef::TypeRef;
  void insert(uint64_t index, const std::string& utf8);
  void insertUtf16(uint64_t index, const std::u16string& s);
  void remove(uint64_t index, uint64_t delLen);
  std::string toString() const;  // UTF-8
  std::u16string toUtf16() const;
};

class Array : public TypeRef {
 public:
  using TypeRef::TypeRef;
  void insert(uint64_t index, const std::vector<Any>& values);
  void push(const Any& value) { insert(length(), {value}); }
  void remove(uint64_t index, uint64_t delLen);
  Value get(uint64_t index) const;
  Array insertArray(uint64_t index);
  Map insertMap(uint64_t index);
  Text insertText(uint64_t index);
  std::string toJson() const;
};

class Map : public TypeRef {
 public:
  using TypeRef::TypeRef;
  void set(const std::string& key, const Any& value);
  void remove(const std::string& key);
  bool has(const std::string& key) const;
  std::optional<Value> get(const std::string& key) const;
  std::vector<std::string> keys() const;
  size_t size() const;
  Array setArray(const std::string& key);
  Map setMap(const std::string& key);
  Text setText(const std::string& key);
  std::string toJson() const;
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

  // Root types, addressed by name. A root's kind is fixed by the first getX()
  // call; accessing it as a different kind throws.
  Text getText(const std::string& name = "default");
  Array getArray(const std::string& name = "default");
  Map getMap(const std::string& name = "default");

  // Apply a Yjs v1 update. Structs with missing causal dependencies are kept
  // pending and retried on subsequent calls. Runs in a transaction: observers
  // and update handlers fire with the given origin and local == false.
  void applyUpdate(const std::vector<uint8_t>& update, const std::string& origin = "");

  // Batch several mutations into one transaction: observers and update
  // handlers fire once, at the end. Nested calls join the outer transaction.
  void transact(const std::function<void()>& fn, const std::string& origin = "");

  // Called at the end of every transaction that changed the doc, with the
  // transaction encoded as an incremental v1 update (what a sync provider
  // forwards) and the transaction origin. Returns a handle for offUpdate().
  using UpdateHandler = std::function<void(const std::vector<uint8_t>&, const std::string&)>;
  uint64_t onUpdate(UpdateHandler fn);
  void offUpdate(uint64_t handle);

  // RAII variant of onUpdate(): the handler detaches when the returned
  // Subscription is destroyed or reset().
  Subscription subscribeUpdates(UpdateHandler fn);

  std::vector<uint8_t> encodeStateAsUpdate() const;
  // remoteStateVector: output of encodeStateVector() on the remote doc.
  std::vector<uint8_t> encodeStateAsUpdate(const std::vector<uint8_t>& remoteStateVector) const;
  std::vector<uint8_t> encodeStateVector() const;

  // Next expected clock for a client (0 if unknown).
  uint64_t getState(uint64_t client) const;

  // True if some received structs/deletes still await missing dependencies.
  bool hasPending() const { return !pending_.empty() || !pendingDs_.empty(); }

  // Number of structs in the store (bounded by document shape, not history).
  size_t structCount() const;

  // JSON dump of every root type (sorted by name), for inspection/testing.
  std::string toJson() const;

 private:
  friend class TypeRef;
  friend class Text;
  friend class Array;
  friend class Map;

  struct Transaction {
    std::map<uint64_t, uint64_t> beforeState;
    std::map<uint64_t, std::vector<DeleteRange>> ds;  // deletes made in this tx
    std::map<YType*, std::set<std::optional<std::string>>> changed;
    std::string origin;
    bool local = true;
  };
  struct TxScope {  // RAII: every mutation path opens an implicit transaction
    Doc* d;
    TxScope(Doc* doc, const std::string& origin, bool local) : d(doc) {
      d->beginTransaction(origin, local);
    }
    ~TxScope() { d->endTransaction(); }
  };
  void beginTransaction(const std::string& origin, bool local);
  void endTransaction();
  void cleanupTransaction(Transaction& tx);
  void addChangedType(YType* t, const std::optional<std::string>& sub);
  static bool txAdds(const Transaction& tx, const Item* it);
  static bool txDeletes(const Transaction& tx, const Item* it);
  std::vector<uint8_t> encodeTransactionUpdate(const Transaction& tx) const;
  void buildDelta(const Transaction& tx, const YType* t, Event& ev) const;
  void buildKeyChange(const Transaction& tx, const YType* t, const std::string& key,
                      Event& ev) const;

  Item* alloc();
  YType* allocType(TypeKind kind);
  YType* rootType(const std::string& name, TypeKind wanted);
  void addStruct(Item* it);
  Item* getItem(const ID& id) const;
  Item* getItemCleanStart(const ID& id);
  Item* getItemCleanEnd(const ID& id);
  Item* splitItem(Item* it, uint64_t diff);
  void deleteItem(Item* s);  // tombstone + record in tx + cascade types
  // Merges v[pos] into v[pos-1] if the yjs merge conditions hold (frees v[pos]).
  bool tryMergeWithLeft(std::vector<Item*>& v, size_t pos);

  bool depsMissing(const Item* it) const;
  void integrate(Item* it, uint64_t offset);
  void integrateAsGc(Item* it, uint64_t offset);
  void integrateLoop();
  // Applies one delete range; returns the not-yet-applicable remainder (len 0 if none).
  DeleteRange applyDeleteRange(uint64_t client, uint64_t clock, uint64_t len);

  // List (sequence) helpers shared by Text and Array.
  struct ListPos {
    Item* left = nullptr;
    Item* right = nullptr;
  };
  ListPos findListPosition(YType* t, uint64_t index);  // splits as needed
  void listInsert(YType* t, uint64_t index, Item* it);  // it: content prefilled
  void listRemove(YType* t, uint64_t index, uint64_t delLen);
  void mapSet(YType* t, const std::string& key, Item* it);
  Item* newItem();  // alloc + local id assignment happens in listInsert/mapSet

  // One encoded struct-section entry: an item (with offset) or a Skip gap.
  struct Emit {
    const Item* it = nullptr;  // nullptr -> Skip of skipLen
    uint64_t offset = 0;
    uint64_t skipLen = 0;
  };
  bool buildClientEmits(uint64_t client, uint64_t fromClock, std::vector<Emit>& out,
                        uint64_t& startClock) const;
  void writeStruct(lib0::Encoder& enc, const Item* it, uint64_t offset) const;
  void writeDeleteSet(lib0::Encoder& enc) const;
  std::vector<uint8_t> encodeStateAsUpdateSv(const std::map<uint64_t, uint64_t>& sv) const;

  void dumpType(std::string& out, const YType* t) const;

  uint64_t clientId_;
  std::map<uint64_t, std::vector<Item*>> store_;          // per client, sorted by clock
  std::map<std::string, YType*> roots_;                   // root types by name
  std::vector<std::unique_ptr<YType>> typeArena_;
  std::unordered_map<Item*, std::unique_ptr<Item>> items_;  // ownership; erased on merge
  std::map<uint64_t, std::deque<Item*>> pending_;         // structs awaiting deps
  std::map<uint64_t, std::vector<DeleteRange>> pendingDs_;  // deletes awaiting structs

  std::unique_ptr<Transaction> tx_;
  int txDepth_ = 0;
  uint64_t nextHandle_ = 1;  // observer / update-handler handles
  std::vector<std::pair<uint64_t, UpdateHandler>> updateHandlers_;
};

}  // namespace ycpp
