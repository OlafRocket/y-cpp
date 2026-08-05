// The Doc: struct store, root types, transactions, update codec entry
// points, observers, and validation.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "ycpp/event.hpp"
#include "ycpp/item.hpp"
#include "ycpp/lib0.hpp"
#include "ycpp/type.hpp"
#include "ycpp/wrappers.hpp"

namespace ycpp {
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
  void applyUpdate(std::span<const uint8_t> update, const std::string& origin = "");

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

  [[nodiscard]] std::vector<uint8_t> encodeStateAsUpdate() const;
  // remoteStateVector: output of encodeStateVector() on the remote doc.
  [[nodiscard]] std::vector<uint8_t> encodeStateAsUpdate(std::span<const uint8_t> remoteStateVector) const;
  [[nodiscard]] std::vector<uint8_t> encodeStateVector() const;

  // Next expected clock for a client (0 if unknown).
  [[nodiscard]] uint64_t getState(uint64_t client) const;

  // True if some received structs/deletes still await missing dependencies.
  bool hasPending() const { return !pending_.empty() || !pendingDs_.empty(); }

  // Number of structs in the store (bounded by document shape, not history).
  size_t structCount() const;

  // Walks every structure and throws std::logic_error if any internal
  // invariant is broken (list symmetry, store contiguity, arena membership,
  // type lengths, map-chain coherence, no orphaned items). Cheap insurance
  // for tests; see setValidateAfterTransactions().
  void validate() const;

  // When enabled (test builds), validate() runs after every transaction, so
  // a broken invariant fails at the mutation that caused it.
  static void setValidateAfterTransactions(bool on) { validateAfterTx_ = on; }

  // JSON dump of every root type (sorted by name), for inspection/testing.
  [[nodiscard]] std::string toJson() const;

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
  // The single place an Item is freed. Checklist — every structure that may
  // hold an Item*, all of which the caller must have unlinked first:
  //   (1) neighbor left/right links, (2) the per-client store_ vector,
  //   (3) YType::map values, (4) YType::start, (5) pending_ queues.
  void releaseItem(Item* it);
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
  // Liveness token for Subscriptions: expires with the Doc.
  std::shared_ptr<void> aliveToken_ = std::make_shared<int>(0);

  inline static bool validateAfterTx_ = false;
};

}  // namespace ycpp
