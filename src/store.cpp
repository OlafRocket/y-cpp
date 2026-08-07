// Struct store surgery: splitting, cleaning, deletion, merging, freeing.
#include "ycpp/ycpp.hpp"

#include <algorithm>
#include <exception>
#include <random>
#include <stdexcept>
#include <unordered_set>

#include "internal.hpp"

namespace ycpp {

Item* Doc::splitItem(Item* it, uint64_t diff) {
  Item* r = alloc();
  r->id = ID{it->id.client, it->id.clock + diff};
  r->origin = ID{it->id.client, it->id.clock + diff - 1};
  r->rightOrigin = it->rightOrigin;  // left keeps its rightOrigin too (as in yjs)
  r->parent = it->parent;
  r->parentName = it->parentName;
  r->parentSub = it->parentSub;
  r->kind = it->kind;
  r->deleted = it->deleted;
  r->integrated = true;
  r->keep = it->keep;
  if (it->redone) r->redone = ID{it->redone->client, it->redone->clock + diff};
  r->len = it->len - diff;
  switch (it->kind) {
    case ContentKind::String:
      r->content = it->str().substr(diff);
      it->str().resize(diff);
      // Splitting inside a surrogate pair produces two U+FFFD, same as yjs.
      if (!it->str().empty() && it->str().back() >= 0xD800 && it->str().back() <= 0xDBFF) {
        it->str().back() = 0xFFFD;
        r->str()[0] = 0xFFFD;
      }
      break;
    case ContentKind::Any:
    case ContentKind::JSON:
      r->content = std::vector<Any>(it->anyVals().begin() + static_cast<long>(diff),
                                    it->anyVals().end());
      it->anyVals().resize(diff);
      break;
    case ContentKind::Deleted:
      break;
    default:
      throw std::runtime_error("ycpp: cannot split this content kind");
  }
  it->len = diff;
  r->right = it->right;
  if (r->right) r->right->left = r;
  it->right = r;
  r->left = it;
  // The split right part may become the current map value holder.
  if (r->parentSub && r->parent) {
    auto mit = r->parent->map.find(*r->parentSub);
    if (mit != r->parent->map.end() && mit->second == it) mit->second = r;
  }
  auto& v = store_[it->id.client];
  v.insert(v.begin() + static_cast<long>(findIndex(v, it->id.clock)) + 1, r);
  return r;
}

Item* Doc::getItemCleanStart(const ID& id) {
  auto& v = store_.at(id.client);
  Item* it = v[findIndex(v, id.clock)];
  if (it->id.clock < id.clock && it->kind != ContentKind::GC) {
    return splitItem(it, id.clock - it->id.clock);
  }
  return it;
}

Item* Doc::getItemCleanEnd(const ID& id) {
  auto& v = store_.at(id.client);
  Item* it = v[findIndex(v, id.clock)];
  if (id.clock != it->id.clock + it->len - 1 && it->kind != ContentKind::GC) {
    splitItem(it, id.clock - it->id.clock + 1);
  }
  return it;
}

void Doc::deleteItem(Item* s) {
  if (s->deleted) return;
  s->deleted = true;
  if (s->countable() && !s->parentSub && s->parent) s->parent->length -= s->len;
  // Content stays intact until transaction cleanup (events may read it);
  // record the deletion and the changed type on the transaction.
  if (tx_) tx_->ds[s->id.client].push_back(DeleteRange{s->id.clock, s->len});
  if (s->parent) addChangedType(s->parent, s->parentSub);
  if (s->kind == ContentKind::Type && s->typeContent()) {
    // ContentType.delete: the whole subtree dies with its container. The
    // type ref itself is kept (we do not convert children to GC structs).
    for (Item* c = s->typeContent()->start; c != nullptr; c = c->right) {
      if (!c->deleted) deleteItem(c);
    }
    for (auto& [k, item] : s->typeContent()->map) {
      if (!item->deleted) deleteItem(item);
    }
  }
}

// Port of yjs Item.mergeWith / tryToMergeWithLefts: absorb v[pos] into
// v[pos-1] when both halves are one contiguous run in the same list state.
bool Doc::tryMergeWithLeft(std::vector<Item*>& v, size_t pos) {
  if (pos == 0 || pos >= v.size()) return false;
  Item* l = v[pos - 1];
  Item* r = v[pos];
  if (l->kind != r->kind || l->deleted != r->deleted) return false;
  switch (l->kind) {
    case ContentKind::String: case ContentKind::Deleted: case ContentKind::GC:
    case ContentKind::Any: case ContentKind::JSON:
      break;
    default:
      return false;  // Binary/Type/Embed/Format/Doc do not merge
  }
  if (l->id.clock + l->len != r->id.clock) return false;
  if (l->redone || r->redone) return false;  // undo provenance must stay addressable
  if (l->kind != ContentKind::GC) {
    if (l->right != r) return false;
    if (!r->origin || !(*r->origin == l->lastId())) return false;
    if (!sameOpt(l->rightOrigin, r->rightOrigin)) return false;
  }
  if (r->keep) l->keep = true;
  if (l->kind == ContentKind::String) l->str() += r->str();
  if (l->kind == ContentKind::Any || l->kind == ContentKind::JSON) {
    l->anyVals().insert(l->anyVals().end(), r->anyVals().begin(), r->anyVals().end());
  }
  l->len += r->len;
  if (l->kind != ContentKind::GC) {
    l->right = r->right;
    if (r->right) r->right->left = l;
  }
  if (r->parentSub && r->parent) {
    auto mit = r->parent->map.find(*r->parentSub);
    if (mit != r->parent->map.end() && mit->second == r) mit->second = l;
  }
  v.erase(v.begin() + static_cast<long>(pos));
  releaseItem(r);
  return true;
}

void Doc::releaseItem(Item* it) { items_.erase(it); }

std::pair<Item*, uint64_t> Doc::followRedone(const ID& id) const {
  ID nextId = id;
  uint64_t diff = 0;
  Item* item = nullptr;
  while (true) {
    if (diff > 0) nextId = ID{nextId.client, nextId.clock + diff};
    item = getItem(nextId);
    diff = nextId.clock - item->id.clock;
    if (item->kind == ContentKind::GC || !item->redone) break;
    nextId = *item->redone;
  }
  return {item, diff};
}

}  // namespace ycpp
