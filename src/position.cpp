// Relative positions: ports of yjs createRelativePositionFromTypeIndex,
// createAbsolutePositionFromRelativePosition (without followRedone — no undo
// yet), and the v1 binary codec.
#include "ycpp/position.hpp"

#include "internal.hpp"

namespace ycpp {

RelativePosition createRelativePosition(const TypeRef& type, uint64_t index, int64_t assoc) {
  RelativePosition r;
  r.assoc = assoc;
  YType* t = type.node();
  auto anchorType = [&] {
    if (t->item != nullptr) r.typeId = t->item->id;
    else r.tname = t->rootName;
  };
  if (assoc < 0) {
    // Left-associated: anchor to the unit left of the position.
    if (index == 0) {
      anchorType();
      return r;
    }
    index--;
  }
  for (Item* n = t->start; n != nullptr; n = n->right) {
    if (!n->deleted && n->countable()) {
      if (n->len > index) {
        r.item = ID{n->id.client, n->id.clock + index};
        return r;
      }
      index -= n->len;
    }
    if (n->right == nullptr && assoc < 0) {
      r.item = n->lastId();
      return r;
    }
  }
  anchorType();
  return r;
}

std::optional<AbsolutePosition> resolveRelativePosition(const RelativePosition& rpos, Doc& doc) {
  YType* type = nullptr;
  uint64_t index = 0;
  if (rpos.item) {
    if (doc.getState(rpos.item->client) <= rpos.item->clock) return std::nullopt;
    const auto& v = doc.store_.at(rpos.item->client);
    Item* right = v[findIndex(v, rpos.item->clock)];
    uint64_t diff = rpos.item->clock - right->id.clock;
    if (right->kind == ContentKind::GC) return std::nullopt;  // anchor collected
    type = right->parent;
    if (type == nullptr) return std::nullopt;
    if (type->item == nullptr || !type->item->deleted) {
      index = (right->deleted || !right->countable())
                  ? 0
                  : diff + (rpos.assoc >= 0 ? 0 : 1);
      for (Item* n = right->left; n != nullptr; n = n->left) {
        if (!n->deleted && n->countable()) index += n->len;
      }
    }
  } else if (rpos.tname) {
    type = doc.rootType(*rpos.tname, TypeKind::Unknown);
    index = rpos.assoc >= 0 ? type->length : 0;
  } else if (rpos.typeId) {
    if (doc.getState(rpos.typeId->client) <= rpos.typeId->clock) return std::nullopt;
    Item* item = doc.getItem(*rpos.typeId);
    if (item->kind != ContentKind::Type || item->typeContent() == nullptr) {
      return std::nullopt;  // type item was garbage collected
    }
    type = item->typeContent();
    index = rpos.assoc >= 0 ? type->length : 0;
  } else {
    return std::nullopt;
  }
  return AbsolutePosition{type, index, rpos.assoc};
}

std::vector<uint8_t> encodeRelativePosition(const RelativePosition& rpos) {
  lib0::Encoder enc;
  if (rpos.item) {
    enc.writeVarUint(0);
    writeID(enc, *rpos.item);
  } else if (rpos.tname) {
    enc.writeUint8(1);
    enc.writeVarString(*rpos.tname);
  } else if (rpos.typeId) {
    enc.writeUint8(2);
    writeID(enc, *rpos.typeId);
  } else {
    throw std::runtime_error("ycpp: relative position without an anchor");
  }
  enc.writeVarInt(rpos.assoc);
  return enc.take();
}

RelativePosition decodeRelativePosition(std::span<const uint8_t> bytes) {
  lib0::Decoder dec(bytes);
  RelativePosition r;
  switch (dec.readVarUint()) {
    case 0: r.item = readID(dec); break;
    case 1: r.tname = dec.readVarString(); break;
    case 2: r.typeId = readID(dec); break;
    default: throw std::runtime_error("ycpp: unknown relative position case");
  }
  r.assoc = dec.hasContent() ? dec.readVarInt() : 0;
  return r;
}

}  // namespace ycpp
