// YATA integration: conflict resolution, pending structs, delete ranges.
#include "ycpp/ycpp.hpp"

#include <algorithm>
#include <exception>
#include <random>
#include <stdexcept>
#include <unordered_set>

#include "internal.hpp"

namespace ycpp {

// ---------------------------------------------------------------- integration

bool Doc::depsMissing(const Item* it) const {
  // Same-client dependencies are guaranteed by per-client clock order.
  if (it->origin && it->origin->client != it->id.client &&
      it->origin->clock >= getState(it->origin->client)) {
    return true;
  }
  if (it->rightOrigin && it->rightOrigin->client != it->id.client &&
      it->rightOrigin->clock >= getState(it->rightOrigin->client)) {
    return true;
  }
  if (it->parentId && it->parentId->client != it->id.client &&
      it->parentId->clock >= getState(it->parentId->client)) {
    return true;
  }
  return false;
}

void Doc::integrateAsGc(Item* it, uint64_t offset) {
  it->id.clock += offset;
  it->len -= offset;
  it->kind = ContentKind::GC;
  it->deleted = true;
  it->content = std::monostate{};
  it->left = it->right = nullptr;
  addStruct(it);
}

// Port of yjs Item.getMissing (neighbor/parent resolution) + Item.integrate
// (YATA conflict resolution and map semantics). `offset` > 0 means the first
// `offset` clock units of this struct are already present in the store.
void Doc::integrate(Item* it, uint64_t offset) {
  if (it->kind == ContentKind::GC) {
    integrateAsGc(it, offset);
    return;
  }

  // Resolve neighbors from origins (splits existing items as needed).
  if (it->origin) it->left = getItemCleanEnd(*it->origin);
  if (it->rightOrigin) it->right = getItemCleanStart(*it->rightOrigin);

  if ((it->left && it->left->kind == ContentKind::GC) ||
      (it->right && it->right->kind == ContentKind::GC)) {
    // A neighbor was garbage-collected: this item can never be positioned,
    // integrate it as a GC filler (as yjs does).
    integrateAsGc(it, offset);
    return;
  }

  // Resolve parent.
  if (!it->parent) {
    if (it->parentName) {
      it->parent = rootType(*it->parentName, TypeKind::Unknown);
    } else if (it->parentId) {
      Item* p = getItem(*it->parentId);
      if (p->kind == ContentKind::Type && p->typeContent()) {
        it->parent = p->typeContent();
      } else {
        // Parent item was garbage-collected: integrate as GC (yjs getMissing).
        integrateAsGc(it, offset);
        return;
      }
    } else if (it->left) {
      it->parent = it->left->parent;
      it->parentSub = it->left->parentSub;
    } else if (it->right) {
      it->parent = it->right->parent;
      it->parentSub = it->right->parentSub;
    }
  }
  if (!it->parent) {
    integrateAsGc(it, offset);
    return;
  }
  YType* parent = it->parent;

  if (offset > 0) {
    it->id.clock += offset;
    Item* l = getItemCleanEnd(ID{it->id.client, it->id.clock - 1});
    if (l->kind == ContentKind::GC) {
      integrateAsGc(it, 0);
      return;
    }
    it->left = l;
    it->origin = l->lastId();
    spliceContentLeft(it, offset);
  }

  // --- YATA conflict resolution (verbatim port of yjs Item.integrate) ---
  if ((!it->left && (!it->right || it->right->left != nullptr)) ||
      (it->left && it->left->right != it->right)) {
    Item* left = it->left;
    Item* o;
    if (left != nullptr) {
      o = left->right;
    } else if (it->parentSub) {
      auto mit = parent->map.find(*it->parentSub);
      o = mit == parent->map.end() ? nullptr : mit->second;
      while (o != nullptr && o->left != nullptr) o = o->left;
    } else {
      o = parent->start;
    }
    std::unordered_set<const Item*> conflictingItems;
    std::unordered_set<const Item*> itemsBeforeOrigin;
    while (o != nullptr && o != it->right) {
      itemsBeforeOrigin.insert(o);
      conflictingItems.insert(o);
      if (sameOpt(it->origin, o->origin)) {
        // case 1: same insertion point, lower client id goes first
        if (o->id.client < it->id.client) {
          left = o;
          conflictingItems.clear();
        } else if (sameOpt(it->rightOrigin, o->rightOrigin)) {
          break;
        }
      } else if (o->origin && itemsBeforeOrigin.count(getItem(*o->origin)) != 0) {
        // case 2
        if (conflictingItems.count(getItem(*o->origin)) == 0) {
          left = o;
          conflictingItems.clear();
        }
      } else {
        break;
      }
      o = o->right;
    }
    it->left = left;
  }

  // Reconnect the linked list / map chain.
  if (it->left) {
    it->right = it->left->right;
    it->left->right = it;
  } else {
    Item* r;
    if (it->parentSub) {
      auto mit = parent->map.find(*it->parentSub);
      r = mit == parent->map.end() ? nullptr : mit->second;
      while (r != nullptr && r->left != nullptr) r = r->left;
    } else {
      r = parent->start;
      parent->start = it;
    }
    it->right = r;
  }
  if (it->right) {
    it->right->left = it;
  } else if (it->parentSub) {
    // This is the new current value for the map key; the previous one dies.
    parent->map[*it->parentSub] = it;
    if (it->left) deleteItem(it->left);
  }

  if (!it->parentSub && it->countable() && !it->deleted) parent->length += it->len;
  addStruct(it);
  addChangedType(parent, it->parentSub);

  // Delete if the parent container is deleted, or if this is a stale map value.
  if ((parent->item != nullptr && parent->item->deleted) ||
      (it->parentSub && it->right != nullptr)) {
    deleteItem(it);
  }
  // Merging happens at transaction cleanup (after events are computed).
}

void Doc::integrateLoop() {
  bool progress = true;
  while (progress) {
    progress = false;
    for (auto& [client, q] : pending_) {
      while (!q.empty()) {
        Item* s = q.front();
        uint64_t state = getState(client);
        if (s->id.clock + s->len <= state) {  // fully known already
          q.pop_front();
          releaseItem(s);
          progress = true;
          continue;
        }
        if (s->id.clock > state) break;  // gap: wait for earlier structs
        if (depsMissing(s)) break;
        q.pop_front();
        integrate(s, state - s->id.clock);
        progress = true;
      }
    }
  }
  for (auto it = pending_.begin(); it != pending_.end();) {
    it = it->second.empty() ? pending_.erase(it) : std::next(it);
  }
}

DeleteRange Doc::applyDeleteRange(uint64_t client, uint64_t clock, uint64_t len) {
  uint64_t state = getState(client);
  uint64_t end = clock + len;
  uint64_t applyEnd = std::min(end, state);
  if (clock < applyEnd) {
    auto& v = store_.at(client);
    size_t idx = findIndex(v, clock);
    Item* s = v[idx];
    if (!s->deleted && s->id.clock < clock) {
      splitItem(s, clock - s->id.clock);
      idx++;
    }
    while (idx < v.size()) {
      s = v[idx++];
      if (s->id.clock >= applyEnd) break;
      if (!s->deleted) {
        if (applyEnd < s->id.clock + s->len) splitItem(s, applyEnd - s->id.clock);
        deleteItem(s);
      }
    }
  }
  uint64_t restStart = std::max(clock, applyEnd);
  return DeleteRange{restStart, end - restStart};
}

}  // namespace ycpp
