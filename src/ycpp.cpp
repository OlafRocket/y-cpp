#include "ycpp/ycpp.hpp"

#include <algorithm>
#include <exception>
#include <random>
#include <stdexcept>
#include <unordered_set>

#include "ycpp/utf.hpp"

namespace ycpp {

namespace {

constexpr uint8_t kBitOrigin = 0x80;      // BIT8: origin present
constexpr uint8_t kBitRightOrigin = 0x40; // BIT7: rightOrigin present
constexpr uint8_t kBitParentSub = 0x20;   // BIT6: parentSub present
constexpr uint8_t kBitsContent = 0x1f;    // BITS5: content ref

bool sameOpt(const std::optional<ID>& a, const std::optional<ID>& b) {
  if (!a && !b) return true;
  if (a && b) return *a == *b;
  return false;
}

void writeID(lib0::Encoder& enc, const ID& id) {
  enc.writeVarUint(id.client);
  enc.writeVarUint(id.clock);
}

ID readID(lib0::Decoder& dec) {
  ID id;
  id.client = dec.readVarUint();
  id.clock = dec.readVarUint();
  return id;
}

// Index of the struct containing `clock` in a sorted struct list.
size_t findIndex(const std::vector<Item*>& v, uint64_t clock) {
  size_t lo = 0, hi = v.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    const Item* m = v[mid];
    if (clock < m->id.clock) {
      hi = mid;
    } else if (clock >= m->id.clock + m->len) {
      lo = mid + 1;
    } else {
      return mid;
    }
  }
  throw std::runtime_error("ycpp: struct not found in store");
}

// Drop the first `offset` content units of an item (used when the prefix of
// an incoming struct is already known).
void spliceContentLeft(Item* it, uint64_t offset) {
  switch (it->kind) {
    case ContentKind::String:
      it->str.erase(0, offset);
      if (!it->str.empty() && it->str[0] >= 0xDC00 && it->str[0] <= 0xDFFF) {
        it->str[0] = 0xFFFD;  // split surrogate pair, same as yjs ContentString
      }
      break;
    case ContentKind::Any:
    case ContentKind::JSON:
      it->anyVals.erase(it->anyVals.begin(),
                        it->anyVals.begin() + static_cast<long>(offset));
      break;
    case ContentKind::Deleted:
      break;
    default:
      if (offset > 0) throw std::runtime_error("ycpp: cannot splice this content");
  }
  it->len -= offset;
}

std::map<uint64_t, uint64_t> decodeStateVector(std::span<const uint8_t> bytes) {
  std::map<uint64_t, uint64_t> sv;
  lib0::Decoder dec(bytes);
  uint64_t n = dec.readVarUint();
  for (uint64_t i = 0; i < n; i++) {
    uint64_t client = dec.readVarUint();
    uint64_t clock = dec.readVarUint();
    sv[client] = clock;
  }
  return sv;
}

// One content unit of an item as a Value.
Value valueAt(const Item* it, uint64_t idx) {
  Value v;
  switch (it->kind) {
    case ContentKind::Type: v.type = it->typeContent; break;
    case ContentKind::Any:
    case ContentKind::JSON:
      v.any = it->anyVals[idx];
      break;
    case ContentKind::Embed:
    case ContentKind::Doc:
      v.any = it->anyVals.empty() ? Any() : it->anyVals[0];
      break;
    case ContentKind::Binary: v.any = Any::binary(it->bin); break;
    case ContentKind::String:
      v.any = Any::of(utf::utf16To8(it->str.substr(idx, 1)));
      break;
    default: break;
  }
  return v;
}

}  // namespace

// ---------------------------------------------------------------- Doc basics

Doc::Doc(std::optional<uint64_t> clientId) {
  if (clientId) {
    clientId_ = *clientId;
  } else {
    std::random_device rd;
    std::mt19937 gen(rd());
    clientId_ = std::uniform_int_distribution<uint32_t>(0, 0x7fffffff)(gen);
  }
}

Doc::~Doc() = default;

Item* Doc::alloc() {
  auto it = std::make_unique<Item>();
  Item* p = it.get();
  items_.emplace(p, std::move(it));
  return p;
}

YType* Doc::allocType(TypeKind kind) {
  typeArena_.push_back(std::make_unique<YType>());
  YType* t = typeArena_.back().get();
  t->kind = kind;
  t->doc = this;
  return t;
}

YType* Doc::rootType(const std::string& name, TypeKind wanted) {
  auto it = roots_.find(name);
  if (it == roots_.end()) {
    YType* t = allocType(wanted);
    t->rootName = name;
    roots_[name] = t;
    return t;
  }
  YType* t = it->second;
  if (t->kind == TypeKind::Unknown) t->kind = wanted;  // late typing of a decoded root
  if (wanted != TypeKind::Unknown && t->kind != wanted) {
    throw std::runtime_error("ycpp: root type '" + name + "' already has a different kind");
  }
  return t;
}

Text Doc::getText(const std::string& name) { return Text(rootType(name, TypeKind::Text)); }
Array Doc::getArray(const std::string& name) { return Array(rootType(name, TypeKind::Array)); }
Map Doc::getMap(const std::string& name) { return Map(rootType(name, TypeKind::Map)); }

size_t Doc::structCount() const {
  size_t n = 0;
  for (const auto& [client, v] : store_) n += v.size();
  return n;
}

void Doc::validate() const {
  auto fail = [](const std::string& msg) {
    throw std::logic_error("ycpp validate: " + msg);
  };
  auto inArena = [&](const Item* it) {
    return items_.find(const_cast<Item*>(it)) != items_.end();
  };

  // Store: contiguous per-client clock ranges, every item arena-owned.
  size_t referenced = 0;
  for (const auto& [client, v] : store_) {
    if (v.empty()) fail("empty store vector for client");
    uint64_t clock = 0;
    for (const Item* it : v) {
      if (!inArena(it)) fail("store item not owned by arena");
      if (it->id.client != client) fail("store item client mismatch");
      if (it->id.clock != clock) fail("clock gap in store");
      if (it->len == 0) fail("zero-length item in store");
      if (!it->integrated) fail("store item not marked integrated");
      if (it->deleted &&
          (it->kind == ContentKind::String || it->kind == ContentKind::Any ||
           it->kind == ContentKind::Binary)) {
        fail("deleted item content not garbage-collected");
      }
      if (!it->deleted && it->kind == ContentKind::String && it->len != it->str.size()) {
        fail("string item length mismatch");
      }
      if (!it->deleted && it->kind == ContentKind::Any && it->len != it->anyVals.size()) {
        fail("any item length mismatch");
      }
      clock = it->id.clock + it->len;
      referenced++;
    }
  }

  // Pending: arena-owned, never integrated.
  for (const auto& [client, q] : pending_) {
    if (q.empty()) fail("empty pending queue kept alive");
    for (const Item* it : q) {
      if (!inArena(it)) fail("pending item not owned by arena");
      if (it->integrated) fail("pending item marked integrated");
      referenced++;
    }
  }
  if (referenced != items_.size()) {
    fail("arena orphans: " + std::to_string(items_.size()) + " owned vs " +
         std::to_string(referenced) + " referenced");
  }

  // Types: list symmetry, parent links, length accounting, map chains.
  for (const auto& tptr : typeArena_) {
    const YType* t = tptr.get();
    uint64_t countable = 0;
    const Item* prev = nullptr;
    for (const Item* n = t->start; n != nullptr; n = n->right) {
      if (!inArena(n)) fail("list node not owned by arena");
      if (n->left != prev) fail("list left/right asymmetry");
      if (n->parent != t) fail("list node parent mismatch");
      if (n->parentSub) fail("map item linked into list chain");
      if (!n->deleted && n->countable()) countable += n->len;
      prev = n;
    }
    if (countable != t->length) fail("type length mismatch");
    for (const auto& [key, val] : t->map) {
      if (val->right != nullptr) fail("map value is not the rightmost item");
      for (const Item* n = val; n != nullptr; n = n->left) {
        if (!inArena(n)) fail("map chain node not owned by arena");
        if (!n->parentSub || *n->parentSub != key) fail("map chain key mismatch");
        if (n->parent != t) fail("map chain parent mismatch");
        if (n->left != nullptr && n->left->right != n) fail("map chain asymmetry");
      }
    }
  }
}

uint64_t Doc::getState(uint64_t client) const {
  auto it = store_.find(client);
  if (it == store_.end() || it->second.empty()) return 0;
  const Item* last = it->second.back();
  return last->id.clock + last->len;
}

void Doc::addStruct(Item* it) {
  auto& v = store_[it->id.client];
  if (!v.empty() && v.back()->id.clock + v.back()->len != it->id.clock) {
    throw std::runtime_error("ycpp: struct store gap (internal error)");
  }
  if (v.empty() && it->id.clock != 0) {
    throw std::runtime_error("ycpp: struct store must start at clock 0 (internal error)");
  }
  v.push_back(it);
  it->integrated = true;
}

Item* Doc::getItem(const ID& id) const {
  auto it = store_.find(id.client);
  if (it == store_.end()) throw std::runtime_error("ycpp: unknown client in store");
  return it->second[findIndex(it->second, id.clock)];
}

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
  r->len = it->len - diff;
  switch (it->kind) {
    case ContentKind::String:
      r->str = it->str.substr(diff);
      it->str.resize(diff);
      // Splitting inside a surrogate pair produces two U+FFFD, same as yjs.
      if (!it->str.empty() && it->str.back() >= 0xD800 && it->str.back() <= 0xDBFF) {
        it->str.back() = 0xFFFD;
        r->str[0] = 0xFFFD;
      }
      break;
    case ContentKind::Any:
    case ContentKind::JSON:
      r->anyVals.assign(it->anyVals.begin() + static_cast<long>(diff), it->anyVals.end());
      it->anyVals.resize(diff);
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
  if (s->kind == ContentKind::Type && s->typeContent) {
    // ContentType.delete: the whole subtree dies with its container. The
    // type ref itself is kept (we do not convert children to GC structs).
    for (Item* c = s->typeContent->start; c != nullptr; c = c->right) {
      if (!c->deleted) deleteItem(c);
    }
    for (auto& [k, item] : s->typeContent->map) {
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
  if (l->kind != ContentKind::GC) {
    if (l->right != r) return false;
    if (!r->origin || !(*r->origin == l->lastId())) return false;
    if (!sameOpt(l->rightOrigin, r->rightOrigin)) return false;
  }
  if (l->kind == ContentKind::String) l->str += r->str;
  if (l->kind == ContentKind::Any || l->kind == ContentKind::JSON) {
    l->anyVals.insert(l->anyVals.end(), r->anyVals.begin(), r->anyVals.end());
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
  it->str.clear();
  it->anyVals.clear();
  it->bin.clear();
  it->typeContent = nullptr;
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
      if (p->kind == ContentKind::Type && p->typeContent) {
        it->parent = p->typeContent;
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

// ---------------------------------------------------------------- applyUpdate

void Doc::applyUpdate(std::span<const uint8_t> update, const std::string& origin) {
  TxScope scope(this, origin, /*local=*/false);
  lib0::Decoder dec(update);

  // Struct section.
  uint64_t numClients = dec.readVarUint();
  for (uint64_t c = 0; c < numClients; c++) {
    uint64_t numStructs = dec.readVarUint();
    uint64_t client = dec.readVarUint();
    uint64_t clock = dec.readVarUint();
    std::deque<Item*> incoming;
    try {
    for (uint64_t i = 0; i < numStructs; i++) {
      uint8_t info = dec.readUint8();
      uint8_t ref = info & kBitsContent;
      if (ref == 10) {  // Skip: a gap in a diff update, occupies clock space only
        clock += dec.readVarUint();
        continue;
      }
      Item* it = alloc();
      it->id = ID{client, clock};
      try {
      if (ref == 0) {  // GC
        it->kind = ContentKind::GC;
        it->deleted = true;
        it->len = dec.readVarUint();
      } else {
        if (info & kBitOrigin) it->origin = readID(dec);
        if (info & kBitRightOrigin) it->rightOrigin = readID(dec);
        if ((info & (kBitOrigin | kBitRightOrigin)) == 0) {
          if (dec.readVarUint() == 1) {
            it->parentName = dec.readVarString();
          } else {
            it->parentId = readID(dec);
          }
          if (info & kBitParentSub) it->parentSub = dec.readVarString();
        }
        switch (ref) {
          case 1:  // ContentDeleted
            it->kind = ContentKind::Deleted;
            it->deleted = true;
            it->len = dec.readVarUint();
            break;
          case 2: {  // ContentJSON (legacy): kept as raw JSON strings
            it->kind = ContentKind::JSON;
            uint64_t n = dec.readVarUint();
            for (uint64_t k = 0; k < n; k++) it->anyVals.push_back(Any::of(dec.readVarString()));
            it->len = n;
            break;
          }
          case 3:  // ContentBinary
            it->kind = ContentKind::Binary;
            it->bin = dec.readBytes(dec.readVarUint());
            it->len = 1;
            break;
          case 4:  // ContentString
            it->kind = ContentKind::String;
            it->str = utf::utf8To16(dec.readVarString());
            it->len = it->str.size();
            break;
          case 5:  // ContentEmbed: raw JSON, opaque
            it->kind = ContentKind::Embed;
            it->anyVals.push_back(Any::of(dec.readVarString()));
            it->len = 1;
            break;
          case 6:  // ContentFormat: attribute key + raw JSON value, opaque
            it->kind = ContentKind::Format;
            it->key = dec.readVarString();
            it->anyVals.push_back(Any::of(dec.readVarString()));
            it->len = 1;
            break;
          case 7: {  // ContentType
            it->kind = ContentKind::Type;
            uint64_t typeRef = dec.readVarUint();
            if (typeRef > 6) {
              throw std::runtime_error("ycpp: unknown type ref " + std::to_string(typeRef));
            }
            YType* t = allocType(static_cast<TypeKind>(typeRef));
            if (typeRef == 3 || typeRef == 5) t->xmlName = dec.readVarString();
            t->item = it;
            it->typeContent = t;
            it->len = 1;
            break;
          }
          case 8: {  // ContentAny
            it->kind = ContentKind::Any;
            uint64_t n = dec.readVarUint();
            it->anyVals.reserve(n);
            for (uint64_t k = 0; k < n; k++) it->anyVals.push_back(anycodec::read(dec));
            it->len = n;
            break;
          }
          case 9:  // ContentDoc (subdocument): preserved, no subdoc semantics
            it->kind = ContentKind::Doc;
            it->key = dec.readVarString();  // guid
            it->anyVals.push_back(anycodec::read(dec));
            it->len = 1;
            break;
          default:
            throw std::runtime_error("ycpp: unsupported content ref " + std::to_string(ref));
        }
      }
      } catch (...) {
        releaseItem(it);  // do not leave a half-decoded orphan in the arena
        throw;
      }
      if (it->len == 0) {
        releaseItem(it);
        continue;
      }
      clock += it->len;
      incoming.push_back(it);
    }
    } catch (...) {
      for (Item* x : incoming) releaseItem(x);  // no orphans on bad updates
      throw;
    }
    if (!incoming.empty()) {
      auto& q = pending_[client];
      q.insert(q.end(), incoming.begin(), incoming.end());
      std::stable_sort(q.begin(), q.end(),
                       [](const Item* a, const Item* b) { return a->id.clock < b->id.clock; });
    }
  }

  integrateLoop();

  // Delete set section; combine with deletes still pending from earlier updates.
  std::vector<std::pair<uint64_t, DeleteRange>> toApply;
  for (auto& [client, ranges] : pendingDs_) {
    for (const auto& r : ranges) toApply.emplace_back(client, r);
  }
  pendingDs_.clear();
  uint64_t dsClients = dec.readVarUint();
  for (uint64_t c = 0; c < dsClients; c++) {
    uint64_t client = dec.readVarUint();
    uint64_t numRanges = dec.readVarUint();
    for (uint64_t i = 0; i < numRanges; i++) {
      uint64_t clock = dec.readVarUint();
      uint64_t len = dec.readVarUint();
      if (len > 0) toApply.emplace_back(client, DeleteRange{clock, len});
    }
  }
  for (auto& [client, r] : toApply) {
    DeleteRange rest = applyDeleteRange(client, r.clock, r.len);
    if (rest.len > 0) pendingDs_[client].push_back(rest);
  }
}

// ------------------------------------------------------------------ encoding

void Doc::writeStruct(lib0::Encoder& enc, const Item* it, uint64_t offset) const {
  if (it->kind == ContentKind::GC) {
    enc.writeUint8(0);
    enc.writeVarUint(it->len - offset);
    return;
  }
  std::optional<ID> origin =
      offset > 0 ? std::optional<ID>(ID{it->id.client, it->id.clock + offset - 1}) : it->origin;
  uint8_t info = static_cast<uint8_t>(it->kind) |
                 (origin ? kBitOrigin : 0) |
                 (it->rightOrigin ? kBitRightOrigin : 0) |
                 (it->parentSub ? kBitParentSub : 0);
  enc.writeUint8(info);
  if (origin) writeID(enc, *origin);
  if (it->rightOrigin) writeID(enc, *it->rightOrigin);
  if (!origin && !it->rightOrigin) {
    if (it->parent) {
      if (it->parent->item != nullptr) {  // nested type: parent addressed by item id
        enc.writeVarUint(0);
        writeID(enc, it->parent->item->id);
      } else {
        enc.writeVarUint(1);
        enc.writeVarString(it->parent->rootName);
      }
    } else if (it->parentName) {
      enc.writeVarUint(1);
      enc.writeVarString(*it->parentName);
    } else if (it->parentId) {
      enc.writeVarUint(0);
      writeID(enc, *it->parentId);
    } else {
      throw std::runtime_error("ycpp: cannot encode item without parent");
    }
    if (it->parentSub) enc.writeVarString(*it->parentSub);
  }
  switch (it->kind) {
    case ContentKind::Deleted:
      enc.writeVarUint(it->len - offset);
      break;
    case ContentKind::JSON:
      enc.writeVarUint(it->len - offset);
      for (uint64_t i = offset; i < it->anyVals.size(); i++) {
        enc.writeVarString(it->anyVals[i].str);
      }
      break;
    case ContentKind::Binary:
      enc.writeVarUint(it->bin.size());
      enc.writeBytes(it->bin);
      break;
    case ContentKind::String:
      enc.writeVarString(utf::utf16To8(it->str.substr(offset)));
      break;
    case ContentKind::Embed:
      enc.writeVarString(it->anyVals[0].str);
      break;
    case ContentKind::Format:
      enc.writeVarString(it->key);
      enc.writeVarString(it->anyVals[0].str);
      break;
    case ContentKind::Type: {
      TypeKind k = it->typeContent ? it->typeContent->kind : TypeKind::Unknown;
      if (k == TypeKind::Unknown) throw std::runtime_error("ycpp: cannot encode untyped ContentType");
      enc.writeVarUint(static_cast<uint8_t>(k));
      if (k == TypeKind::XmlElement || k == TypeKind::XmlHook) {
        enc.writeVarString(it->typeContent->xmlName);
      }
      break;
    }
    case ContentKind::Any:
      enc.writeVarUint(it->len - offset);
      for (uint64_t i = offset; i < it->anyVals.size(); i++) anycodec::write(enc, it->anyVals[i]);
      break;
    case ContentKind::Doc:
      enc.writeVarString(it->key);
      anycodec::write(enc, it->anyVals[0]);
      break;
    default:
      throw std::runtime_error("ycpp: cannot encode content kind");
  }
}

// Collects everything to encode for one client: the integrated structs from
// `fromClock`, then structs still pending (not yet integrable), with Skip
// structs filling clock gaps — mirroring how yjs merges store.pendingStructs
// into encodeStateAsUpdate so that no received data is lost on re-encode.
bool Doc::buildClientEmits(uint64_t client, uint64_t fromClock, std::vector<Emit>& out,
                           uint64_t& startClock) const {
  const std::vector<Item*>* v = nullptr;
  auto sit = store_.find(client);
  if (sit != store_.end() && !sit->second.empty()) v = &sit->second;
  uint64_t state = v ? getState(client) : 0;
  bool haveNext = false;
  uint64_t next = 0;
  if (v && fromClock < state) {
    uint64_t clock = std::max(fromClock, (*v)[0]->id.clock);
    size_t start = findIndex(*v, clock);
    startClock = clock;
    out.push_back(Emit{(*v)[start], clock - (*v)[start]->id.clock, 0});
    for (size_t i = start + 1; i < v->size(); i++) out.push_back(Emit{(*v)[i], 0, 0});
    next = state;
    haveNext = true;
  }
  auto pit = pending_.find(client);
  if (pit != pending_.end()) {
    for (const Item* p : pit->second) {
      if (!haveNext) {
        next = p->id.clock;
        startClock = next;
        haveNext = true;
      }
      if (p->id.clock + p->len <= next) continue;  // fully covered already
      uint64_t off = 0;
      if (p->id.clock > next) {
        out.push_back(Emit{nullptr, 0, p->id.clock - next});
      } else {
        off = next - p->id.clock;
      }
      out.push_back(Emit{p, off, 0});
      next = p->id.clock + p->len;
    }
  }
  return !out.empty();
}

void Doc::writeDeleteSet(lib0::Encoder& enc) const {
  // Derived from the store (yjs createDeleteSetFromStructStore), plus delete
  // ranges still pending, so re-encoded state loses nothing.
  std::map<uint64_t, std::vector<DeleteRange>> ds;
  for (const auto& [client, v] : store_) {
    std::vector<DeleteRange> ranges;
    for (const Item* it : v) {
      if (!it->deleted) continue;
      if (!ranges.empty() && ranges.back().clock + ranges.back().len == it->id.clock) {
        ranges.back().len += it->len;
      } else {
        ranges.push_back(DeleteRange{it->id.clock, it->len});
      }
    }
    if (!ranges.empty()) ds[client] = std::move(ranges);
  }
  for (const auto& [client, ranges] : pendingDs_) {
    auto& dst = ds[client];
    dst.insert(dst.end(), ranges.begin(), ranges.end());
    std::sort(dst.begin(), dst.end(),
              [](const DeleteRange& a, const DeleteRange& b) { return a.clock < b.clock; });
    std::vector<DeleteRange> merged;
    for (const auto& r : dst) {
      if (!merged.empty() && r.clock <= merged.back().clock + merged.back().len) {
        merged.back().len = std::max(merged.back().len, r.clock + r.len - merged.back().clock);
      } else {
        merged.push_back(r);
      }
    }
    dst = std::move(merged);
  }
  enc.writeVarUint(ds.size());
  for (auto rit = ds.rbegin(); rit != ds.rend(); ++rit) {  // descending client order
    enc.writeVarUint(rit->first);
    enc.writeVarUint(rit->second.size());
    for (const auto& r : rit->second) {
      enc.writeVarUint(r.clock);
      enc.writeVarUint(r.len);
    }
  }
}

std::vector<uint8_t> Doc::encodeStateAsUpdateSv(const std::map<uint64_t, uint64_t>& sv) const {
  lib0::Encoder enc;
  // Candidate clients: everything in the store plus clients with pending structs.
  std::map<uint64_t, uint64_t> fromClocks;  // client -> encode-from clock
  for (const auto& [client, v] : store_) {
    auto it = sv.find(client);
    fromClocks[client] = it == sv.end() ? 0 : it->second;
  }
  for (const auto& [client, q] : pending_) {
    if (fromClocks.find(client) == fromClocks.end()) fromClocks[client] = 0;
  }
  std::map<uint64_t, std::pair<std::vector<Emit>, uint64_t>> perClient;
  for (const auto& [client, fromClock] : fromClocks) {
    std::vector<Emit> emits;
    uint64_t startClock = 0;
    if (buildClientEmits(client, fromClock, emits, startClock)) {
      perClient[client] = {std::move(emits), startClock};
    }
  }
  enc.writeVarUint(perClient.size());
  for (auto rit = perClient.rbegin(); rit != perClient.rend(); ++rit) {  // descending client order
    const auto& [emits, startClock] = rit->second;
    enc.writeVarUint(emits.size());
    enc.writeVarUint(rit->first);
    enc.writeVarUint(startClock);
    for (const Emit& e : emits) {
      if (e.it == nullptr) {  // Skip struct: gap before pending structs
        enc.writeUint8(10);
        enc.writeVarUint(e.skipLen);
      } else {
        writeStruct(enc, e.it, e.offset);
      }
    }
  }
  writeDeleteSet(enc);
  return enc.take();
}

std::vector<uint8_t> Doc::encodeStateAsUpdate() const {
  return encodeStateAsUpdateSv({});
}

std::vector<uint8_t> Doc::encodeStateAsUpdate(std::span<const uint8_t> remoteStateVector) const {
  return encodeStateAsUpdateSv(decodeStateVector(remoteStateVector));
}

std::vector<uint8_t> Doc::encodeStateVector() const {
  lib0::Encoder enc;
  enc.writeVarUint(store_.size());
  for (auto rit = store_.rbegin(); rit != store_.rend(); ++rit) {
    enc.writeVarUint(rit->first);
    enc.writeVarUint(getState(rit->first));
  }
  return enc.take();
}

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
        cur.str += item->str;
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

  // 2. Garbage-collect droppable content deleted in this transaction.
  for (const auto& [client, ranges] : tx.ds) {
    auto sit = store_.find(client);
    if (sit == store_.end()) continue;
    auto& v = sit->second;
    for (const DeleteRange& r : ranges) {
      size_t idx = findIndex(v, r.clock);
      while (idx < v.size() && v[idx]->id.clock < r.clock + r.len) {
        Item* s = v[idx++];
        if (!s->deleted) continue;
        switch (s->kind) {
          case ContentKind::String:
            s->str.clear();
            s->str.shrink_to_fit();
            s->kind = ContentKind::Deleted;
            break;
          case ContentKind::Any:
            s->anyVals.clear();
            s->anyVals.shrink_to_fit();
            s->kind = ContentKind::Deleted;
            break;
          case ContentKind::Binary:
            s->bin.clear();
            s->bin.shrink_to_fit();
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

// ------------------------------------------------------------ list/map ops

Doc::ListPos Doc::findListPosition(YType* t, uint64_t index) {
  ListPos pos;
  pos.right = t->start;
  uint64_t rem = index;
  while (pos.right && rem > 0) {
    if (!pos.right->deleted && pos.right->countable()) {
      if (rem < pos.right->len) {
        splitItem(pos.right, rem);
        rem = 0;
      } else {
        rem -= pos.right->len;
      }
    }
    pos.left = pos.right;
    pos.right = pos.right->right;
  }
  return pos;
}

void Doc::listInsert(YType* t, uint64_t index, Item* it) {
  if (index > t->length) throw std::out_of_range("ycpp: insert index out of range");
  TxScope scope(this, "", /*local=*/true);
  ListPos pos = findListPosition(t, index);
  it->id = ID{clientId_, getState(clientId_)};
  it->origin = pos.left ? std::optional<ID>(pos.left->lastId()) : std::nullopt;
  it->rightOrigin = pos.right ? std::optional<ID>(pos.right->id) : std::nullopt;
  it->left = pos.left;
  it->right = pos.right;
  it->parent = t;
  integrate(it, 0);
}

void Doc::listRemove(YType* t, uint64_t index, uint64_t delLen) {
  if (delLen == 0) return;
  if (index + delLen > t->length) throw std::out_of_range("ycpp: remove range out of range");
  TxScope scope(this, "", /*local=*/true);
  ListPos pos = findListPosition(t, index);
  Item* right = pos.right;
  uint64_t remaining = delLen;
  while (remaining > 0 && right) {
    if (!right->deleted && right->countable()) {
      if (remaining < right->len) splitItem(right, remaining);
      deleteItem(right);
      remaining -= right->len;
    }
    right = right->right;
  }
  // Compaction happens at transaction cleanup.
}

void Doc::mapSet(YType* t, const std::string& key, Item* it) {
  TxScope scope(this, "", /*local=*/true);
  auto mit = t->map.find(key);
  Item* left = mit == t->map.end() ? nullptr : mit->second;
  it->id = ID{clientId_, getState(clientId_)};
  it->origin = left ? std::optional<ID>(left->lastId()) : std::nullopt;
  it->left = left;
  it->right = nullptr;
  it->parent = t;
  it->parentSub = key;
  integrate(it, 0);
}

// ---------------------------------------------------------------------- Text

void Text::insert(uint64_t index, const std::string& utf8) {
  insertUtf16(index, utf::utf8To16(utf8));
}

void Text::insertUtf16(uint64_t index, const std::u16string& s) {
  if (s.empty()) return;
  Doc* d = t_->doc;
  Item* it = d->alloc();
  it->kind = ContentKind::String;
  it->str = s;
  it->len = s.size();
  d->listInsert(t_, index, it);
}

void Text::remove(uint64_t index, uint64_t delLen) {
  t_->doc->listRemove(t_, index, delLen);
}

std::u16string Text::toUtf16() const {
  std::u16string out;
  out.reserve(t_->length);
  for (const Item* it = t_->start; it != nullptr; it = it->right) {
    if (!it->deleted && it->kind == ContentKind::String) out += it->str;
  }
  return out;
}

std::string Text::toString() const {
  return utf::utf16To8(toUtf16());
}

// --------------------------------------------------------------------- Array

void Array::insert(uint64_t index, const std::vector<Any>& values) {
  if (values.empty()) return;
  Doc* d = t_->doc;
  Item* it = d->alloc();
  it->kind = ContentKind::Any;
  it->anyVals = values;
  it->len = values.size();
  d->listInsert(t_, index, it);
}

void Array::remove(uint64_t index, uint64_t delLen) {
  t_->doc->listRemove(t_, index, delLen);
}

Value Array::get(uint64_t index) const {
  for (const Item* it = t_->start; it != nullptr; it = it->right) {
    if (it->deleted || !it->countable()) continue;
    if (index < it->len) return valueAt(it, index);
    index -= it->len;
  }
  throw std::out_of_range("ycpp: array index out of range");
}

Array Array::insertArray(uint64_t index) {
  Doc* d = t_->doc;
  Item* it = d->alloc();
  it->kind = ContentKind::Type;
  it->typeContent = d->allocType(TypeKind::Array);
  it->typeContent->item = it;
  it->len = 1;
  d->listInsert(t_, index, it);
  return Array(it->typeContent);
}

Map Array::insertMap(uint64_t index) {
  Doc* d = t_->doc;
  Item* it = d->alloc();
  it->kind = ContentKind::Type;
  it->typeContent = d->allocType(TypeKind::Map);
  it->typeContent->item = it;
  it->len = 1;
  d->listInsert(t_, index, it);
  return Map(it->typeContent);
}

Text Array::insertText(uint64_t index) {
  Doc* d = t_->doc;
  Item* it = d->alloc();
  it->kind = ContentKind::Type;
  it->typeContent = d->allocType(TypeKind::Text);
  it->typeContent->item = it;
  it->len = 1;
  d->listInsert(t_, index, it);
  return Text(it->typeContent);
}

std::string Array::toJson() const {
  std::string out;
  t_->doc->dumpType(out, t_);
  return out;
}

// ----------------------------------------------------------------------- Map

void Map::set(const std::string& key, const Any& value) {
  Doc* d = t_->doc;
  Item* it = d->alloc();
  it->kind = ContentKind::Any;
  it->anyVals = {value};
  it->len = 1;
  d->mapSet(t_, key, it);
}

void Map::remove(const std::string& key) {
  auto mit = t_->map.find(key);
  if (mit != t_->map.end() && !mit->second->deleted) {
    Doc::TxScope scope(t_->doc, "", /*local=*/true);
    t_->doc->deleteItem(mit->second);
  }
}

bool Map::has(const std::string& key) const {
  auto mit = t_->map.find(key);
  return mit != t_->map.end() && !mit->second->deleted;
}

std::optional<Value> Map::get(const std::string& key) const {
  auto mit = t_->map.find(key);
  if (mit == t_->map.end() || mit->second->deleted) return std::nullopt;
  const Item* it = mit->second;
  return valueAt(it, it->len - 1);
}

std::vector<std::string> Map::keys() const {
  std::vector<std::string> out;
  for (const auto& [k, item] : t_->map) {
    if (!item->deleted) out.push_back(k);
  }
  return out;
}

size_t Map::size() const { return keys().size(); }

Array Map::setArray(const std::string& key) {
  Doc* d = t_->doc;
  Item* it = d->alloc();
  it->kind = ContentKind::Type;
  it->typeContent = d->allocType(TypeKind::Array);
  it->typeContent->item = it;
  it->len = 1;
  d->mapSet(t_, key, it);
  return Array(it->typeContent);
}

Map Map::setMap(const std::string& key) {
  Doc* d = t_->doc;
  Item* it = d->alloc();
  it->kind = ContentKind::Type;
  it->typeContent = d->allocType(TypeKind::Map);
  it->typeContent->item = it;
  it->len = 1;
  d->mapSet(t_, key, it);
  return Map(it->typeContent);
}

Text Map::setText(const std::string& key) {
  Doc* d = t_->doc;
  Item* it = d->alloc();
  it->kind = ContentKind::Type;
  it->typeContent = d->allocType(TypeKind::Text);
  it->typeContent->item = it;
  it->len = 1;
  d->mapSet(t_, key, it);
  return Text(it->typeContent);
}

std::string Map::toJson() const {
  std::string out;
  t_->doc->dumpType(out, t_);
  return out;
}

// ---------------------------------------------------------------------- JSON

void Doc::dumpType(std::string& out, const YType* t) const {
  TypeKind kind = t->kind;
  if (kind == TypeKind::Unknown) {
    // Heuristic for roots never typed locally: map entries -> object,
    // string-only list -> text, otherwise array.
    bool hasMap = false;
    for (const auto& [k, item] : t->map) {
      if (!item->deleted) hasMap = true;
    }
    bool allString = true;
    for (const Item* it = t->start; it != nullptr; it = it->right) {
      if (!it->deleted && it->countable() && it->kind != ContentKind::String) allString = false;
    }
    kind = hasMap ? TypeKind::Map : (allString && t->start ? TypeKind::Text : TypeKind::Array);
  }
  if (kind == TypeKind::Text || kind == TypeKind::XmlText) {
    json::escape(out, Text(const_cast<YType*>(t)).toString());
    return;
  }
  if (kind == TypeKind::Map || kind == TypeKind::XmlHook) {
    out.push_back('{');
    bool first = true;
    for (const auto& [k, item] : t->map) {
      if (item->deleted) continue;
      if (!first) out.push_back(',');
      first = false;
      json::escape(out, k);
      out.push_back(':');
      Value v = valueAt(item, item->len - 1);
      if (v.isType()) dumpType(out, v.type);
      else json::dump(out, v.any);
    }
    out.push_back('}');
    return;
  }
  // Array-like (Array, XmlFragment, XmlElement children).
  out.push_back('[');
  bool first = true;
  for (const Item* it = t->start; it != nullptr; it = it->right) {
    if (it->deleted || !it->countable()) continue;
    for (uint64_t i = 0; i < it->len; i++) {
      if (!first) out.push_back(',');
      first = false;
      Value v = valueAt(it, i);
      if (v.isType()) dumpType(out, v.type);
      else json::dump(out, v.any);
    }
  }
  out.push_back(']');
}

std::string Doc::toJson() const {
  std::string out = "{";
  bool first = true;
  for (const auto& [name, t] : roots_) {
    if (!first) out.push_back(',');
    first = false;
    json::escape(out, name);
    out.push_back(':');
    dumpType(out, t);
  }
  out.push_back('}');
  return out;
}

}  // namespace ycpp
