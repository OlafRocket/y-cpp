// The Item struct (yjs "struct"): the atom of CRDT content, plus the
// content-kind and ID vocabulary of the Yjs v1 encoding.
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "ycpp/any.hpp"

namespace ycpp {
struct YType;
struct ID {
  uint64_t client = 0;
  uint64_t clock = 0;
  bool operator==(const ID& o) const { return client == o.client && clock == o.clock; }
};

// Content refs of the Yjs v1 encoding. Text/Any/Binary/Type carry the data
// model; JSON/Embed/Format/Doc are decoded, preserved, and re-encoded
// opaquely (no dedicated API); everything occupies clock space.
enum class ContentKind : uint8_t {
  GC = 0,       // deleted range from a peer that garbage-collected (no content)
  Deleted = 1,  // ContentDeleted: tombstone with length only
  JSON = 2,     // legacy ContentJSON (raw JSON strings, opaque)
  Binary = 3,   // ContentBinary: one Uint8Array value
  String = 4,   // ContentString: run of text, length in UTF-16 code units
  Embed = 5,    // ContentEmbed (raw JSON, opaque)
  Format = 6,   // ContentFormat (rich-text attribute, opaque, not countable)
  Type = 7,     // ContentType: a nested shared type
  Any = 8,      // ContentAny: run of lib0 "Any" values
  Doc = 9,      // ContentDoc (subdocument reference, opaque)
};

struct Item {
  // Boxed payload for the opaque content kinds (Embed/Format/Doc): they are
  // rare, so keeping them behind a pointer keeps every Item small.
  struct Opaque {
    std::string key;  // Format attribute key / Doc guid
    std::string raw;  // raw JSON payload (Embed/Format)
    Any any;          // Doc options
  };
  // Content storage: the alternative must match `kind` (accessors throw
  // std::bad_variant_access on mismatch). monostate covers GC/Deleted.
  using Content = std::variant<std::monostate, std::u16string, std::vector<Any>,
                               std::vector<uint8_t>, YType*, std::unique_ptr<Opaque>>;

  ID id;
  uint64_t len = 0;
  std::optional<ID> origin;       // ID of item to the left at insertion time
  std::optional<ID> rightOrigin;  // ID of item to the right at insertion time
  Item* left = nullptr;
  Item* right = nullptr;
  YType* parent = nullptr;
  std::optional<std::string> parentName;  // root type key ("ykey"), from decode
  std::optional<ID> parentId;             // parent addressed by item ID, from decode
  std::optional<std::string> parentSub;   // map key (Y.Map entries)
  ContentKind kind = ContentKind::String;
  Content content;
  bool deleted = false;
  bool integrated = false;
  // Undo support (yjs): redone points at the item that re-created this one
  // after an undo; keep exempts a tombstone from content GC while an undo
  // stack still references it.
  std::optional<ID> redone;
  bool keep = false;

  ID lastId() const { return ID{id.client, id.clock + len - 1}; }
  bool countable() const {
    switch (kind) {
      case ContentKind::String: case ContentKind::Any: case ContentKind::Binary:
      case ContentKind::Type: case ContentKind::JSON: case ContentKind::Embed:
      case ContentKind::Doc:
        return true;
      default:
        return false;
    }
  }

  std::u16string& str() { return std::get<std::u16string>(content); }
  const std::u16string& str() const { return std::get<std::u16string>(content); }
  std::vector<Any>& anyVals() { return std::get<std::vector<Any>>(content); }
  const std::vector<Any>& anyVals() const { return std::get<std::vector<Any>>(content); }
  std::vector<uint8_t>& bin() { return std::get<std::vector<uint8_t>>(content); }
  const std::vector<uint8_t>& bin() const { return std::get<std::vector<uint8_t>>(content); }
  YType* typeContent() const {
    auto* p = std::get_if<YType*>(&content);
    return p == nullptr ? nullptr : *p;
  }
  Opaque& opaque() { return *std::get<std::unique_ptr<Opaque>>(content); }
  const Opaque& opaque() const { return *std::get<std::unique_ptr<Opaque>>(content); }
};

}  // namespace ycpp
