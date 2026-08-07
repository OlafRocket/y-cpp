// Transactions, event computation, observers, subscriptions.
#include "ycpp/ycpp.hpp"

#include <algorithm>
#include <exception>
#include <random>
#include <stdexcept>
#include <unordered_set>

#include "internal.hpp"

namespace ycpp {

// -------------------------------------------------------------- transactions

void Doc::beginTransaction(const std::string& origin, bool local) {
  if (txDepth_++ == 0) {
    tx_ = std::make_unique<Transaction>();
    tx_->origin = origin;
    tx_->local = local;
    for (const auto& [client, v] : store_) tx_->beforeState[client] = getState(client);
  }
}

void Doc::endTransaction() {
  if (--txDepth_ == 0) {
    std::unique_ptr<Transaction> tx = std::move(tx_);
    cleanupTransaction(*tx);
  }
}

void Doc::transact(const std::function<void()>& fn, const std::string& origin) {
  TxScope scope(this, origin, /*local=*/true);
  fn();
}

// yjs addChangedTypeToTransaction: only report types that existed before the
// transaction and are not deleted (a brand-new nested type's changes are
// covered by its parent's insert event).
void Doc::addChangedType(YType* t, const std::optional<std::string>& sub) {
  if (!tx_) return;
  const Item* item = t->item;
  bool track = item == nullptr;
  if (!track) {
    auto b = tx_->beforeState.find(item->id.client);
    uint64_t before = b == tx_->beforeState.end() ? 0 : b->second;
    track = item->id.clock < before && !item->deleted;
  }
  if (track) tx_->changed[t].insert(sub);
}

// yjs YEvent.adds: created during this transaction.
bool Doc::txAdds(const Transaction& tx, const Item* it) {
  auto b = tx.beforeState.find(it->id.client);
  uint64_t before = b == tx.beforeState.end() ? 0 : b->second;
  return it->id.clock >= before;
}

// yjs YEvent.deletes: deleted during this transaction (tx.ds is sorted/merged).
bool Doc::txDeletes(const Transaction& tx, const Item* it) {
  auto d = tx.ds.find(it->id.client);
  if (d == tx.ds.end()) return false;
  const auto& rs = d->second;
  size_t lo = 0, hi = rs.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (rs[mid].clock <= it->id.clock) lo = mid + 1;
    else hi = mid;
  }
  if (lo == 0) return false;
  const DeleteRange& r = rs[lo - 1];
  return it->id.clock < r.clock + r.len;
}

// Port of yjs YEvent.changes (list delta): classify every item in list order
// as inserted / deleted / retained relative to this transaction.
void Doc::buildDelta(const Transaction& tx, const YType* t, Event& ev) const {
  DeltaOp cur;
  bool has = false;
  auto flush = [&] {
    if (has) {
      ev.delta.push_back(std::move(cur));
      cur = DeltaOp();
      has = false;
    }
  };
  auto begin = [&](DeltaOp::Kind k) {
    if (!has || cur.kind != k) {
      flush();
      cur.kind = k;
      has = true;
    }
  };
  for (const Item* item = t->start; item != nullptr; item = item->right) {
    if (item->deleted) {
      if (txDeletes(tx, item) && !txAdds(tx, item)) {
        begin(DeltaOp::Kind::Delete);
        cur.length += item->len;
      }  // else: deleted before the tx, or added+deleted within it — invisible
    } else if (txAdds(tx, item)) {
      begin(DeltaOp::Kind::Insert);
      cur.length += item->len;
      if (item->kind == ContentKind::String) {
        cur.str += item->str();
      } else {
        for (uint64_t i = 0; i < item->len; i++) cur.values.push_back(valueAt(item, i));
      }
    } else {
      begin(DeltaOp::Kind::Retain);
      cur.length += item->len;
    }
  }
  if (has && cur.kind != DeltaOp::Kind::Retain) flush();  // drop trailing retain
}

// Port of yjs YEvent.keys: action + oldValue for one changed map key.
void Doc::buildKeyChange(const Transaction& tx, const YType* t, const std::string& key,
                         Event& ev) const {
  auto mit = t->map.find(key);
  if (mit == t->map.end()) return;
  const Item* item = mit->second;
  MapChange change;
  if (txAdds(tx, item)) {
    const Item* prev = item->left;
    while (prev != nullptr && txAdds(tx, prev)) prev = prev->left;
    if (item->deleted) {
      if (prev != nullptr && txDeletes(tx, prev)) {
        change.action = MapChange::Action::Delete;
        change.oldValue = valueAt(prev, prev->len - 1);
      } else {
        return;  // added and deleted within the transaction: invisible
      }
    } else if (prev != nullptr && txDeletes(tx, prev)) {
      change.action = MapChange::Action::Update;
      change.oldValue = valueAt(prev, prev->len - 1);
    } else {
      change.action = MapChange::Action::Add;
    }
  } else if (txDeletes(tx, item)) {
    change.action = MapChange::Action::Delete;
    change.oldValue = valueAt(item, item->len - 1);
  } else {
    return;  // nop
  }
  ev.keys[key] = std::move(change);
}

// The transaction encoded as an incremental v1 update: structs added during
// the transaction plus its delete set (yjs writeUpdateMessageFromTransaction).
std::vector<uint8_t> Doc::encodeTransactionUpdate(const Transaction& tx) const {
  lib0::Encoder enc;
  std::map<uint64_t, uint64_t> sm;  // client -> encode-from clock
  for (const auto& [client, v] : store_) {
    auto b = tx.beforeState.find(client);
    uint64_t before = b == tx.beforeState.end() ? 0 : b->second;
    if (getState(client) > before) sm[client] = before;
  }
  enc.writeVarUint(sm.size());
  for (auto rit = sm.rbegin(); rit != sm.rend(); ++rit) {  // descending client order
    const auto& v = store_.at(rit->first);
    uint64_t clock = std::max(rit->second, v[0]->id.clock);
    size_t start = findIndex(v, clock);
    enc.writeVarUint(v.size() - start);
    enc.writeVarUint(rit->first);
    enc.writeVarUint(clock);
    writeStruct(enc, v[start], clock - v[start]->id.clock);
    for (size_t i = start + 1; i < v.size(); i++) writeStruct(enc, v[i], 0);
  }
  enc.writeVarUint(tx.ds.size());
  for (auto rit = tx.ds.rbegin(); rit != tx.ds.rend(); ++rit) {
    enc.writeVarUint(rit->first);
    enc.writeVarUint(rit->second.size());
    for (const DeleteRange& r : rit->second) {
      enc.writeVarUint(r.clock);
      enc.writeVarUint(r.len);
    }
  }
  return enc.take();
}

// yjs cleanupTransactions order: finalize ds -> observers -> gc -> merge ->
// doc "update" handlers.
void Doc::cleanupTransaction(Transaction& tx) {
  // Finalize the transaction delete set (sort + merge overlapping ranges).
  for (auto& [client, ranges] : tx.ds) {
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

  // 1. Type observers (content is still un-GC'd so events can carry it).
  if (!tx.changed.empty()) {
    auto changedCopy = tx.changed;
    for (const auto& [type, subs] : changedCopy) {
      if (type->observers.empty()) continue;
      Event ev;
      ev.target = type;
      ev.origin = tx.origin;
      ev.local = tx.local;
      if (subs.count(std::nullopt) != 0) buildDelta(tx, type, ev);
      for (const auto& sub : subs) {
        if (sub) buildKeyChange(tx, type, *sub, ev);
      }
      auto observers = type->observers;  // copy: callbacks may unobserve
      for (const auto& [handle, fn] : observers) fn(ev);
    }
  }

  // 1b. afterTransaction handlers (the UndoManager hook): they may mark
  // items keep before garbage collection runs.
  if (!afterTxHandlers_.empty()) {
    TransactionEvent ev;
    ev.origin = tx.origin;
    ev.local = tx.local;
    ev.beforeState = &tx.beforeState;
    for (const auto& [client, v] : store_) ev.afterState[client] = getState(client);
    ev.deleteSet = &tx.ds;
    for (const auto& [type, subs] : tx.changed) ev.changedTypes.push_back(type);
    auto handlers = afterTxHandlers_;
    for (const auto& [handle, fn] : handlers) fn(ev);
  }

  // 2. Garbage-collect droppable content deleted in this transaction.
  for (const auto& [client, ranges] : tx.ds) {
    auto sit = store_.find(client);
    if (sit == store_.end()) continue;
    auto& v = sit->second;
    for (const DeleteRange& r : ranges) {
      size_t idx = findIndex(v, r.clock);
      while (idx < v.size() && v[idx]->id.clock < r.clock + r.len) {
        Item* s = v[idx++];
        if (!s->deleted || s->keep) continue;  // keep: an undo stack needs the content
        switch (s->kind) {
          case ContentKind::String:
          case ContentKind::Any:
          case ContentKind::Binary:
            s->content = std::monostate{};
            s->kind = ContentKind::Deleted;
            break;
          default:
            break;  // Type refs and opaque content are kept
        }
      }
    }
  }

  // 3. Compaction: merge across this transaction's delete ranges and new structs.
  for (const auto& [client, ranges] : tx.ds) {
    auto sit = store_.find(client);
    if (sit == store_.end()) continue;
    auto& v = sit->second;
    for (const DeleteRange& r : ranges) {
      size_t pos = findIndex(v, r.clock);
      while (pos < v.size() && v[pos]->id.clock <= r.clock + r.len) {
        if (!tryMergeWithLeft(v, pos)) pos++;
      }
    }
  }
  for (auto& [client, v] : store_) {
    if (v.empty()) continue;
    auto b = tx.beforeState.find(client);
    uint64_t before = b == tx.beforeState.end() ? 0 : b->second;
    if (getState(client) <= before) continue;
    size_t pos = findIndex(v, std::max(before, v[0]->id.clock));
    while (pos < v.size()) {
      if (!tryMergeWithLeft(v, pos)) pos++;
    }
  }

  // 4. Doc update handlers.
  bool changedAny = !tx.ds.empty();
  if (!changedAny) {
    for (const auto& [client, v] : store_) {
      auto b = tx.beforeState.find(client);
      uint64_t before = b == tx.beforeState.end() ? 0 : b->second;
      if (getState(client) > before) {
        changedAny = true;
        break;
      }
    }
  }
  if (changedAny && !updateHandlers_.empty()) {
    std::vector<uint8_t> update = encodeTransactionUpdate(tx);
    auto handlers = updateHandlers_;  // copy: callbacks may offUpdate
    for (const auto& [handle, fn] : handlers) fn(update, tx.origin);
  }

  if (validateAfterTx_ && std::uncaught_exceptions() == 0) validate();
}

uint64_t Doc::onAfterTransaction(TransactionHandler fn) {
  uint64_t handle = nextHandle_++;
  afterTxHandlers_.emplace_back(handle, std::move(fn));
  return handle;
}

void Doc::offAfterTransaction(uint64_t handle) {
  afterTxHandlers_.erase(
      std::remove_if(afterTxHandlers_.begin(), afterTxHandlers_.end(),
                     [&](const auto& p) { return p.first == handle; }),
      afterTxHandlers_.end());
}

uint64_t Doc::onUpdate(UpdateHandler fn) {
  uint64_t handle = nextHandle_++;
  updateHandlers_.emplace_back(handle, std::move(fn));
  return handle;
}

void Doc::offUpdate(uint64_t handle) {
  updateHandlers_.erase(
      std::remove_if(updateHandlers_.begin(), updateHandlers_.end(),
                     [&](const auto& p) { return p.first == handle; }),
      updateHandlers_.end());
}

void Subscription::reset() {
  // Only touch the doc's structures while its liveness token still exists;
  // if the Doc died first, there is nothing left to detach from.
  if (auto alive = alive_.lock()) {
    if (type_ != nullptr) {
      TypeRef(type_).unobserve(handle_);
    } else if (doc_ != nullptr) {
      doc_->offUpdate(handle_);
    }
  }
  type_ = nullptr;
  doc_ = nullptr;
  handle_ = 0;
  alive_.reset();
}

Subscription TypeRef::subscribe(Observer fn) {
  Subscription sub;
  sub.type_ = t_;
  sub.handle_ = observe(std::move(fn));
  sub.alive_ = t_->doc->aliveToken_;
  return sub;
}

Subscription Doc::subscribeUpdates(UpdateHandler fn) {
  Subscription sub;
  sub.doc_ = this;
  sub.handle_ = onUpdate(std::move(fn));
  sub.alive_ = aliveToken_;
  return sub;
}

uint64_t TypeRef::observe(Observer fn) {
  uint64_t handle = t_->doc->nextHandle_++;
  t_->observers.emplace_back(handle, std::move(fn));
  return handle;
}

void TypeRef::unobserve(uint64_t handle) {
  auto& obs = t_->observers;
  obs.erase(std::remove_if(obs.begin(), obs.end(),
                           [&](const auto& p) { return p.first == handle; }),
            obs.end());
}

}  // namespace ycpp
