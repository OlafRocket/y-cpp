#include "ycpp/ycpp.hpp"

#include <algorithm>
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

// Drop the first `offset` UTF-16 units of an item's content (used when the
// prefix of an incoming struct is already known).
void spliceContentLeft(Item* it, uint64_t offset) {
  if (it->kind == ContentKind::String) {
    it->str.erase(0, offset);
    if (!it->str.empty() && it->str[0] >= 0xDC00 && it->str[0] <= 0xDFFF) {
      it->str[0] = 0xFFFD;  // split surrogate pair, same as yjs ContentString
    }
  }
  it->len -= offset;
}

std::map<uint64_t, uint64_t> decodeStateVector(const std::vector<uint8_t>& bytes) {
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
  items_.push_back(std::make_unique<Item>());
  return items_.back().get();
}

Text& Doc::getText(const std::string& name) {
  auto it = types_.find(name);
  if (it == types_.end()) {
    auto t = std::make_unique<Text>();
    t->name = name;
    t->doc = this;
    it = types_.emplace(name, std::move(t)).first;
  }
  return *it->second;
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
  if (it->kind == ContentKind::String) {
    r->str = it->str.substr(diff);
    it->str.resize(diff);
    // Splitting inside a surrogate pair produces two U+FFFD, same as yjs.
    if (!it->str.empty() && it->str.back() >= 0xD800 && it->str.back() <= 0xDBFF) {
      it->str.back() = 0xFFFD;
      r->str[0] = 0xFFFD;
    }
  }
  it->len = diff;
  r->right = it->right;
  if (r->right) r->right->left = r;
  it->right = r;
  r->left = it;
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

void Doc::markDeleted(Item* s) {
  if (s->deleted) return;
  s->deleted = true;
  if (s->countable() && s->parent) s->parent->length -= s->len;
}

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
  it->left = it->right = nullptr;
  addStruct(it);
}

// Port of yjs Item.getMissing (neighbor/parent resolution) + Item.integrate
// (YATA conflict resolution). `offset` > 0 means the first `offset` clock
// units of this struct are already present in the store.
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
      it->parent = &getText(*it->parentName);
    } else if (it->parentId) {
      throw std::runtime_error("ycpp: non-root parent types are not supported (text-only build)");
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
  if (it->parentSub) {
    throw std::runtime_error("ycpp: parentSub (Y.Map keys) is not supported (text-only build)");
  }

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
    Item* o = left ? left->right : it->parent->start;
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

  // Reconnect the linked list.
  if (it->left) {
    it->right = it->left->right;
    it->left->right = it;
  } else {
    it->right = it->parent->start;
    it->parent->start = it;
  }
  if (it->right) it->right->left = it;

  if (it->countable() && !it->deleted) it->parent->length += it->len;
  addStruct(it);
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
        markDeleted(s);
      }
    }
  }
  uint64_t restStart = std::max(clock, applyEnd);
  return DeleteRange{restStart, end - restStart};
}

// ---------------------------------------------------------------- applyUpdate

void Doc::applyUpdate(const std::vector<uint8_t>& update) {
  lib0::Decoder dec(update);

  // Struct section.
  uint64_t numClients = dec.readVarUint();
  for (uint64_t c = 0; c < numClients; c++) {
    uint64_t numStructs = dec.readVarUint();
    uint64_t client = dec.readVarUint();
    uint64_t clock = dec.readVarUint();
    auto& q = pending_[client];
    for (uint64_t i = 0; i < numStructs; i++) {
      uint8_t info = dec.readUint8();
      uint8_t ref = info & kBitsContent;
      if (ref == 10) {  // Skip: a gap in a diff update, occupies clock space only
        clock += dec.readVarUint();
        continue;
      }
      Item* it = alloc();
      it->id = ID{client, clock};
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
          case 4:  // ContentString
            it->kind = ContentKind::String;
            it->str = utf::utf8To16(dec.readVarString());
            it->len = it->str.size();
            break;
          default:
            throw std::runtime_error(
                "ycpp: unsupported content ref " + std::to_string(ref) +
                " (minimal build supports plain text only)");
        }
      }
      if (it->len == 0) continue;
      clock += it->len;
      q.push_back(it);
    }
    std::stable_sort(q.begin(), q.end(),
                     [](const Item* a, const Item* b) { return a->id.clock < b->id.clock; });
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
      enc.writeVarUint(1);
      enc.writeVarString(it->parent->name);
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
    case ContentKind::String:
      enc.writeVarString(utf::utf16To8(it->str.substr(offset)));
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

std::vector<uint8_t> Doc::encodeStateAsUpdate(const std::vector<uint8_t>& remoteStateVector) const {
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

// ---------------------------------------------------------------------- Text

void Text::insert(uint64_t index, const std::string& utf8) {
  insertUtf16(index, utf::utf8To16(utf8));
}

void Text::insertUtf16(uint64_t index, const std::u16string& s) {
  if (s.empty()) return;
  if (index > length) throw std::out_of_range("ycpp: insert index out of range");
  Doc& d = *doc;
  Item* left = nullptr;
  Item* right = start;
  uint64_t rem = index;
  while (right && rem > 0) {
    if (!right->deleted && right->countable()) {
      if (rem < right->len) {
        d.splitItem(right, rem);
        rem = 0;
      } else {
        rem -= right->len;
      }
    }
    left = right;
    right = right->right;
  }
  Item* it = d.alloc();
  it->id = ID{d.clientId(), d.getState(d.clientId())};
  it->kind = ContentKind::String;
  it->str = s;
  it->len = s.size();
  it->origin = left ? std::optional<ID>(left->lastId()) : std::nullopt;
  it->rightOrigin = right ? std::optional<ID>(right->id) : std::nullopt;
  it->left = left;
  it->right = right;
  it->parent = this;
  it->parentName = name;
  d.integrate(it, 0);
}

void Text::remove(uint64_t index, uint64_t delLen) {
  if (delLen == 0) return;
  if (index + delLen > length) throw std::out_of_range("ycpp: remove range out of range");
  Doc& d = *doc;
  Item* right = start;
  uint64_t rem = index;
  while (right && rem > 0) {
    if (!right->deleted && right->countable()) {
      if (rem < right->len) {
        d.splitItem(right, rem);
        rem = 0;
      } else {
        rem -= right->len;
      }
    }
    right = right->right;
  }
  uint64_t remaining = delLen;
  while (remaining > 0 && right) {
    if (!right->deleted && right->countable()) {
      if (remaining < right->len) d.splitItem(right, remaining);
      d.markDeleted(right);
      remaining -= right->len;
    }
    right = right->right;
  }
}

std::u16string Text::toUtf16() const {
  std::u16string out;
  out.reserve(length);
  for (const Item* it = start; it != nullptr; it = it->right) {
    if (!it->deleted && it->kind == ContentKind::String) out += it->str;
  }
  return out;
}

std::string Text::toString() const {
  return utf::utf16To8(toUtf16());
}

}  // namespace ycpp
