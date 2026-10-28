// Port of yjs UndoManager (src/utils/UndoManager.js) and the redo machinery
// from Item.js (redoItem, keepItem, followRedone lives in store.cpp).
#include "ycpp/undo.hpp"

#include <algorithm>
#include <atomic>

#include "internal.hpp"

namespace ycpp {

namespace {

std::atomic<uint64_t> gUndoManagerCounter{1};

}  // namespace

UndoManager::UndoManager(Doc& doc, const std::vector<TypeRef>& scope)
    : UndoManager(doc, scope, Options{}) {}

UndoManager::UndoManager(Doc& doc, const std::vector<TypeRef>& scope, Options options)
    : doc_(doc),
      trackedOrigins_(std::move(options.trackedOrigins)),
      undoOrigin_("undo-manager:" + std::to_string(gUndoManagerCounter++)),
      captureTimeoutMs_(options.captureTimeoutMs) {
  for (const TypeRef& t : scope) scope_.push_back(t.node());
  txHandle_ = doc_.onAfterTransaction(
      [this](const Doc::TransactionEvent& ev) { handleTransaction(ev); });
}

UndoManager::~UndoManager() { doc_.offAfterTransaction(txHandle_); }

// ------------------------------------------------------------- ds helpers

bool UndoManager::dsContains(const DsMap& ds, const ID& id) {
  auto it = ds.find(id.client);
  if (it == ds.end()) return false;
  for (const DeleteRange& r : it->second) {
    if (id.clock >= r.clock && id.clock < r.clock + r.len) return true;
  }
  return false;
}

void UndoManager::dsAdd(DsMap& ds, uint64_t client, uint64_t clock, uint64_t len) {
  ds[client].push_back(DeleteRange{clock, len});
}

void UndoManager::dsSortAndMerge(DsMap& ds) {
  for (auto& [client, ranges] : ds) {
    std::sort(ranges.begin(), ranges.end(),
              [](const DeleteRange& a, const DeleteRange& b) { return a.clock < b.clock; });
    std::vector<DeleteRange> merged;
    for (const DeleteRange& r : ranges) {
      if (!merged.empty() && r.clock <= merged.back().clock + merged.back().len) {
        merged.back().len = std::max(merged.back().len, r.clock + r.len - merged.back().clock);
      } else {
        merged.push_back(r);
      }
    }
    ranges = std::move(merged);
  }
}

void UndoManager::dsMergeInto(DsMap& into, const DsMap& from) {
  for (const auto& [client, ranges] : from) {
    auto& dst = into[client];
    dst.insert(dst.end(), ranges.begin(), ranges.end());
  }
  dsSortAndMerge(into);
}

// ------------------------------------------------------------- scope/keep

bool UndoManager::itemInScope(Item* it) const {  // yjs isParentOf over the scope
  while (it != nullptr) {
    YType* p = it->parent;
    if (p == nullptr) return false;
    for (YType* s : scope_) {
      if (s == p) return true;
    }
    it = p->item;
  }
  return false;
}

bool UndoManager::typeInScope(YType* t) const {
  for (YType* cur = t; cur != nullptr; cur = cur->item ? cur->item->parent : nullptr) {
    for (YType* s : scope_) {
      if (s == cur) return true;
    }
  }
  return false;
}

void UndoManager::keepItem(Item* it, bool keep) {  // yjs keepItem
  while (it != nullptr && it->keep != keep) {
    it->keep = keep;
    it = it->parent ? it->parent->item : nullptr;
  }
}

// Walks all live structs covered by the ranges, splitting at boundaries so
// the callback sees exactly-covered items (yjs iterateDeletedStructs).
void UndoManager::iterateDeletedStructs(const DsMap& ds, const std::function<void(Item*)>& fn) {
  for (const auto& [client, ranges] : ds) {
    for (const DeleteRange& r : ranges) {
      uint64_t state = doc_.getState(client);
      uint64_t end = std::min(r.clock + r.len, state);
      if (r.clock >= end) continue;
      doc_.getItemCleanStart(ID{client, r.clock});
      uint64_t clock = r.clock;
      while (clock < end) {
        Item* s = doc_.getItem(ID{client, clock});
        if (s->id.clock + s->len > end) {
          doc_.getItemCleanStart(ID{client, end});
          s = doc_.getItem(ID{client, clock});
        }
        clock = s->id.clock + s->len;
        if (s->kind != ContentKind::GC) fn(s);
      }
    }
  }
}

bool UndoManager::isDeletedByStacks(const ID& id) const {
  for (const auto* stack : {&undoStack_, &redoStack_}) {
    for (const StackItem& si : *stack) {
      if (dsContains(si.deletions, id)) return true;
    }
  }
  return false;
}

// ------------------------------------------------------------- capture

void UndoManager::handleTransaction(const Doc::TransactionEvent& ev) {
  bool tracked = ev.origin == undoOrigin_ || trackedOrigins_.count(ev.origin) > 0;
  if (!tracked) return;
  bool scoped = false;
  for (YType* t : ev.changedTypes) {
    if (typeInScope(t)) {
      scoped = true;
      break;
    }
  }
  if (!scoped) return;

  bool undoing = undoing_;
  bool redoing = redoing_;
  auto& stack = undoing ? redoStack_ : undoStack_;
  if (undoing) {
    stopCapturing();  // next undo must not be appended to the last item
  } else if (!redoing) {
    clear(false, true);  // a fresh user change invalidates the redo stack
  }

  DsMap insertions;
  for (const auto& [client, endClock] : ev.afterState) {
    auto b = ev.beforeState->find(client);
    uint64_t startClock = b == ev.beforeState->end() ? 0 : b->second;
    if (endClock > startClock) dsAdd(insertions, client, startClock, endClock - startClock);
  }
  auto now = std::chrono::steady_clock::now();
  bool group =
      lastChangeSet_ && !stack.empty() && !undoing && !redoing &&
      std::chrono::duration_cast<std::chrono::milliseconds>(now - lastChange_).count() <
          static_cast<int64_t>(captureTimeoutMs_);
  if (group) {
    dsMergeInto(stack.back().deletions, *ev.deleteSet);
    dsMergeInto(stack.back().insertions, insertions);
  } else {
    StackItem si;
    si.deletions = *ev.deleteSet;
    si.insertions = std::move(insertions);
    dsSortAndMerge(si.deletions);
    stack.push_back(std::move(si));
  }
  if (!undoing && !redoing) {
    lastChange_ = now;
    lastChangeSet_ = true;
  }
  // Deleted structs must survive GC while an undo could revive them.
  iterateDeletedStructs(*ev.deleteSet, [&](Item* it) {
    if (itemInScope(it)) keepItem(it, true);
  });
}

void UndoManager::clear(bool clearUndoStack, bool clearRedoStack) {
  auto clearStack = [&](std::vector<StackItem>& stack) {
    for (StackItem& si : stack) {
      iterateDeletedStructs(si.deletions, [&](Item* it) {
        if (itemInScope(it)) keepItem(it, false);
      });
    }
    stack.clear();
  };
  if (clearUndoStack) clearStack(undoStack_);
  if (clearRedoStack) clearStack(redoStack_);
}

// ------------------------------------------------------------- undo/redo

bool UndoManager::undo() {
  undoing_ = true;
  bool performed = popStackItem(undoStack_);
  undoing_ = false;
  return performed;
}

bool UndoManager::redo() {
  redoing_ = true;
  bool performed = popStackItem(redoStack_);
  redoing_ = false;
  return performed;
}

bool UndoManager::popStackItem(std::vector<StackItem>& stack) {  // yjs popStackItem
  bool performedChange = false;
  doc_.transact(
      [&] {
        while (!stack.empty() && !performedChange) {
          StackItem si = std::move(stack.back());
          stack.pop_back();
          std::vector<Item*> toRedoOrdered;
          std::unordered_set<Item*> toRedo;
          std::vector<Item*> toDelete;
          // Undo the insertions: delete what this group inserted (following
          // redone chains to the current live incarnation).
          iterateDeletedStructs(si.insertions, [&](Item* s) {
            Item* item = s;
            if (item->redone) {
              auto [followed, diff] = doc_.followRedone(item->id);
              item = followed;
              if (diff > 0) {
                item = doc_.getItemCleanStart(ID{item->id.client, item->id.clock + diff});
              }
            }
            if (!item->deleted && itemInScope(item)) toDelete.push_back(item);
          });
          // Undo the deletions: revive what this group deleted (unless it was
          // also inserted by this group — created and deleted together).
          iterateDeletedStructs(si.deletions, [&](Item* s) {
            if (itemInScope(s) && !dsContains(si.insertions, s->id) &&
                toRedo.insert(s).second) {
              toRedoOrdered.push_back(s);
            }
          });
          for (Item* s : toRedoOrdered) {
            performedChange = redoItem(s, toRedo, si.insertions) != nullptr || performedChange;
          }
          // Delete in reverse order so children go before parents.
          for (auto it = toDelete.rbegin(); it != toDelete.rend(); ++it) {
            doc_.deleteItem(*it);
            performedChange = true;
          }
        }
      },
      undoOrigin_);
  return performedChange;
}

Item* UndoManager::redoItem(Item* item, const std::unordered_set<Item*>& redoItems,
                            const DsMap& itemsToDelete) {  // yjs redoItem
  Doc& d = doc_;
  uint64_t ownClient = d.clientId();
  if (item->redone) return d.getItemCleanStart(*item->redone);

  Item* parentItem = item->parent ? item->parent->item : nullptr;
  Item* left = nullptr;
  Item* right = nullptr;
  // Make sure the parent is redone.
  if (parentItem != nullptr && parentItem->deleted) {
    if (!parentItem->redone &&
        (redoItems.count(parentItem) == 0 ||
         redoItem(parentItem, redoItems, itemsToDelete) == nullptr)) {
      return nullptr;
    }
    while (parentItem->redone) parentItem = d.getItemCleanStart(*parentItem->redone);
  }
  YType* parentType = parentItem == nullptr ? item->parent : parentItem->typeContent();
  if (parentType == nullptr) return nullptr;

  auto parentOf = [](Item* it) -> Item* { return it->parent ? it->parent->item : nullptr; };
  if (!item->parentSub) {
    // List item: re-insert at the old position, tracing redone neighbors.
    left = item->left;
    right = item;
    while (left != nullptr) {
      Item* leftTrace = left;
      while (leftTrace != nullptr && parentOf(leftTrace) != parentItem) {
        leftTrace = leftTrace->redone ? d.getItemCleanStart(*leftTrace->redone) : nullptr;
      }
      if (leftTrace != nullptr && parentOf(leftTrace) == parentItem) {
        left = leftTrace;
        break;
      }
      left = left->left;
    }
    while (right != nullptr) {
      Item* rightTrace = right;
      while (rightTrace != nullptr && parentOf(rightTrace) != parentItem) {
        rightTrace = rightTrace->redone ? d.getItemCleanStart(*rightTrace->redone) : nullptr;
      }
      if (rightTrace != nullptr && parentOf(rightTrace) == parentItem) {
        right = rightTrace;
        break;
      }
      right = right->right;
    }
  } else {
    // Map item.
    right = nullptr;
    if (item->right != nullptr) {
      left = item;
      // Walk over rights that this undo (or another stack entry) deletes.
      while (left != nullptr && left->right != nullptr &&
             (left->right->redone || dsContains(itemsToDelete, left->right->id) ||
              isDeletedByStacks(left->right->id))) {
        left = left->right;
        while (left->redone) left = d.getItemCleanStart(*left->redone);
      }
      if (left != nullptr && left->right != nullptr) {
        return nullptr;  // conflicts with a newer change from another client
      }
    } else {
      auto mit = parentType->map.find(*item->parentSub);
      left = mit == parentType->map.end() ? nullptr : mit->second;
    }
    // Drop a cross-parent left: its origin would make the encoder omit the
    // parent info and remote peers would attach the redone item to the old,
    // deleted parent (yjs #757, v13.6.31).
    if (left != nullptr && parentOf(left) != parentItem) {
      auto mit = parentType->map.find(*item->parentSub);
      left = mit == parentType->map.end() ? nullptr : mit->second;
    }
  }

  Item* redone = d.alloc();
  redone->id = ID{ownClient, d.getState(ownClient)};
  redone->left = left;
  redone->origin = left ? std::optional<ID>(left->lastId()) : std::nullopt;
  redone->right = right;
  redone->rightOrigin = right ? std::optional<ID>(right->id) : std::nullopt;
  redone->parent = parentType;
  redone->parentSub = item->parentSub;
  redone->kind = item->kind;
  redone->content = copyContent(*item, redone);
  redone->len = item->len;
  if (redone->kind == ContentKind::Deleted) redone->deleted = true;
  item->redone = redone->id;
  keepItem(redone, true);
  d.integrate(redone, 0);
  return redone;
}

// A fresh copy of an item's content for the re-created item (yjs
// AbstractContent.copy). Type content becomes a new empty type of the same
// kind; its children are revived through their own redo entries.
Item::Content UndoManager::copyContent(const Item& src, Item* owner) {
  Doc& d = doc_;
  switch (src.kind) {
    case ContentKind::String:
      return src.str();
    case ContentKind::Any:
    case ContentKind::JSON:
      return src.anyVals();
    case ContentKind::Binary:
      return src.bin();
    case ContentKind::Type: {
      YType* t = d.allocType(src.typeContent()->kind);
      t->xmlName = src.typeContent()->xmlName;
      t->item = owner;
      return t;
    }
    case ContentKind::Embed:
    case ContentKind::Format:
    case ContentKind::Doc:
      return std::make_unique<Item::Opaque>(src.opaque());
    default:
      return std::monostate{};
  }
}

}  // namespace ycpp
