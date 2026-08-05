// Yjs v1 update codec: applyUpdate decode and state/diff encoding.
#include "ycpp/ycpp.hpp"

#include <algorithm>
#include <exception>
#include <random>
#include <stdexcept>
#include <unordered_set>

#include "internal.hpp"

namespace ycpp {

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
            std::vector<Any> vals;
            vals.reserve(n);
            for (uint64_t k = 0; k < n; k++) vals.push_back(Any::of(dec.readVarString()));
            it->content = std::move(vals);
            it->len = n;
            break;
          }
          case 3:  // ContentBinary
            it->kind = ContentKind::Binary;
            it->content = dec.readBytes(dec.readVarUint());
            it->len = 1;
            break;
          case 4:  // ContentString
            it->kind = ContentKind::String;
            it->content = utf::utf8To16(dec.readVarString());
            it->len = it->str().size();
            break;
          case 5:  // ContentEmbed: raw JSON, opaque
            it->kind = ContentKind::Embed;
            it->content = std::make_unique<Item::Opaque>();
            it->opaque().raw = dec.readVarString();
            it->len = 1;
            break;
          case 6:  // ContentFormat: attribute key + raw JSON value, opaque
            it->kind = ContentKind::Format;
            it->content = std::make_unique<Item::Opaque>();
            it->opaque().key = dec.readVarString();
            it->opaque().raw = dec.readVarString();
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
            it->content = t;
            it->len = 1;
            break;
          }
          case 8: {  // ContentAny
            it->kind = ContentKind::Any;
            uint64_t n = dec.readVarUint();
            std::vector<Any> vals;
            vals.reserve(n);
            for (uint64_t k = 0; k < n; k++) vals.push_back(anycodec::read(dec));
            it->content = std::move(vals);
            it->len = n;
            break;
          }
          case 9:  // ContentDoc (subdocument): preserved, no subdoc semantics
            it->kind = ContentKind::Doc;
            it->content = std::make_unique<Item::Opaque>();
            it->opaque().key = dec.readVarString();  // guid
            it->opaque().any = anycodec::read(dec);
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
      for (uint64_t i = offset; i < it->anyVals().size(); i++) {
        enc.writeVarString(it->anyVals()[i].str());
      }
      break;
    case ContentKind::Binary:
      enc.writeVarUint(it->bin().size());
      enc.writeBytes(it->bin());
      break;
    case ContentKind::String:
      enc.writeVarString(utf::utf16To8(it->str().substr(offset)));
      break;
    case ContentKind::Embed:
      enc.writeVarString(it->opaque().raw);
      break;
    case ContentKind::Format:
      enc.writeVarString(it->opaque().key);
      enc.writeVarString(it->opaque().raw);
      break;
    case ContentKind::Type: {
      TypeKind k = it->typeContent() ? it->typeContent()->kind : TypeKind::Unknown;
      if (k == TypeKind::Unknown) throw std::runtime_error("ycpp: cannot encode untyped ContentType");
      enc.writeVarUint(static_cast<uint8_t>(k));
      if (k == TypeKind::XmlElement || k == TypeKind::XmlHook) {
        enc.writeVarString(it->typeContent()->xmlName);
      }
      break;
    }
    case ContentKind::Any:
      enc.writeVarUint(it->len - offset);
      for (uint64_t i = offset; i < it->anyVals().size(); i++) anycodec::write(enc, it->anyVals()[i]);
      break;
    case ContentKind::Doc:
      enc.writeVarString(it->opaque().key);
      anycodec::write(enc, it->opaque().any);
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

}  // namespace ycpp
