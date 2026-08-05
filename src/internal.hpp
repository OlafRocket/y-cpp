// Shared internal helpers for the ycpp implementation files. Not installed.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ycpp/utf.hpp"
#include "ycpp/ycpp.hpp"

namespace ycpp {


constexpr uint8_t kBitOrigin = 0x80;      // BIT8: origin present
constexpr uint8_t kBitRightOrigin = 0x40; // BIT7: rightOrigin present
constexpr uint8_t kBitParentSub = 0x20;   // BIT6: parentSub present
constexpr uint8_t kBitsContent = 0x1f;    // BITS5: content ref

inline bool sameOpt(const std::optional<ID>& a, const std::optional<ID>& b) {
  if (!a && !b) return true;
  if (a && b) return *a == *b;
  return false;
}

inline void writeID(lib0::Encoder& enc, const ID& id) {
  enc.writeVarUint(id.client);
  enc.writeVarUint(id.clock);
}

inline ID readID(lib0::Decoder& dec) {
  ID id;
  id.client = dec.readVarUint();
  id.clock = dec.readVarUint();
  return id;
}

// Index of the struct containing `clock` in a sorted struct list.
inline size_t findIndex(const std::vector<Item*>& v, uint64_t clock) {
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
inline void spliceContentLeft(Item* it, uint64_t offset) {
  switch (it->kind) {
    case ContentKind::String:
      it->str().erase(0, offset);
      if (!it->str().empty() && it->str()[0] >= 0xDC00 && it->str()[0] <= 0xDFFF) {
        it->str()[0] = 0xFFFD;  // split surrogate pair, same as yjs ContentString
      }
      break;
    case ContentKind::Any:
    case ContentKind::JSON:
      it->anyVals().erase(it->anyVals().begin(),
                        it->anyVals().begin() + static_cast<long>(offset));
      break;
    case ContentKind::Deleted:
      break;
    default:
      if (offset > 0) throw std::runtime_error("ycpp: cannot splice this content");
  }
  it->len -= offset;
}

inline std::map<uint64_t, uint64_t> decodeStateVector(std::span<const uint8_t> bytes) {
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
inline Value valueAt(const Item* it, uint64_t idx) {
  Value v;
  switch (it->kind) {
    case ContentKind::Type: v.type = it->typeContent(); break;
    case ContentKind::Any:
    case ContentKind::JSON:
      v.any = it->anyVals()[idx];
      break;
    case ContentKind::Embed:
      v.any = Any::of(it->opaque().raw);
      break;
    case ContentKind::Doc:
      v.any = it->opaque().any;
      break;
    case ContentKind::Binary: v.any = Any::binary(it->bin()); break;
    case ContentKind::String:
      v.any = Any::of(utf::utf16To8(it->str().substr(idx, 1)));
      break;
    default: break;
  }
  return v;
}

}  // namespace ycpp
