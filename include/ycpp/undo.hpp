// UndoManager (yjs UndoManager): selective, collaborative-safe undo/redo.
//
// Tracks the transactions whose origin is in `trackedOrigins` (by default
// only local, untagged transactions) and whose changes touch the scoped
// types (or their subtrees). undo() reverts the most recent captured group —
// and only those changes: remote edits that interleaved are left intact,
// which is the property that makes undo correct in a collaborative document.
// Edits closer together than captureTimeoutMs are captured as one group
// (call stopCapturing() to force a boundary, e.g. on Enter).
//
// Undoing a deletion revives content: a fresh item is inserted with the same
// content, linked to the original through the item's `redone` id, which
// relative positions and later undos follow (yjs followRedone).
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "ycpp/doc.hpp"
#include "ycpp/wrappers.hpp"

namespace ycpp {

class UndoManager {
 public:
  struct Options {
    uint64_t captureTimeoutMs = 500;         // yjs default
    std::set<std::string> trackedOrigins = {""};  // local, untagged transactions
  };

  UndoManager(Doc& doc, const std::vector<TypeRef>& scope);
  UndoManager(Doc& doc, const std::vector<TypeRef>& scope, Options options);
  ~UndoManager();
  UndoManager(const UndoManager&) = delete;
  UndoManager& operator=(const UndoManager&) = delete;

  // Reverts / re-applies the most recent captured group. Returns true if a
  // change was performed. Must not be called from inside a transaction.
  bool undo();
  bool redo();

  [[nodiscard]] bool canUndo() const { return !undoStack_.empty(); }
  [[nodiscard]] bool canRedo() const { return !redoStack_.empty(); }
  [[nodiscard]] size_t undoStackSize() const { return undoStack_.size(); }
  [[nodiscard]] size_t redoStackSize() const { return redoStack_.size(); }

  // The next tracked change starts a new capture group.
  void stopCapturing() { lastChangeSet_ = false; }

  // Drops stacks (releasing keep flags so tombstones become collectable).
  void clear(bool clearUndoStack = true, bool clearRedoStack = true);

 private:
  using DsMap = std::map<uint64_t, std::vector<DeleteRange>>;
  struct StackItem {
    DsMap deletions;   // what the captured transactions deleted
    DsMap insertions;  // what they inserted (clock ranges)
  };

  void handleTransaction(const Doc::TransactionEvent& ev);
  bool popStackItem(std::vector<StackItem>& stack);
  Item* redoItem(Item* item, const std::unordered_set<Item*>& redoItems,
                 const DsMap& itemsToDelete);
  Item::Content copyContent(const Item& src, Item* owner);
  void iterateDeletedStructs(const DsMap& ds, const std::function<void(Item*)>& fn);
  void keepItem(Item* it, bool keep);
  bool itemInScope(Item* it) const;
  bool typeInScope(YType* t) const;
  bool isDeletedByStacks(const ID& id) const;
  static bool dsContains(const DsMap& ds, const ID& id);
  static void dsAdd(DsMap& ds, uint64_t client, uint64_t clock, uint64_t len);
  static void dsSortAndMerge(DsMap& ds);
  static void dsMergeInto(DsMap& into, const DsMap& from);

  Doc& doc_;
  std::vector<YType*> scope_;
  std::set<std::string> trackedOrigins_;
  std::string undoOrigin_;  // origin tag of this manager's own transactions
  uint64_t captureTimeoutMs_;
  std::vector<StackItem> undoStack_;
  std::vector<StackItem> redoStack_;
  bool undoing_ = false;
  bool redoing_ = false;
  bool lastChangeSet_ = false;
  std::chrono::steady_clock::time_point lastChange_{};
  uint64_t txHandle_ = 0;
};

}  // namespace ycpp
