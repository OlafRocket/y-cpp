// Relative positions (yjs RelativePosition): content-anchored positions that
// survive concurrent edits — the right way to model carets, selections, and
// annotations. Encode/decode is byte-compatible with yjs, so positions can
// travel in awareness states between C++ and JS peers.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ycpp/doc.hpp"
#include "ycpp/item.hpp"
#include "ycpp/wrappers.hpp"

namespace ycpp {

struct RelativePosition {
  std::optional<ID> item;           // anchored at a content unit
  std::optional<std::string> tname;  // or at a root type (list end)
  std::optional<ID> typeId;          // or at a nested type (list end)
  // assoc >= 0: stick to the character right of the position (an insert at
  // the position pushes the anchor right); assoc < 0: stick to the left.
  int64_t assoc = 0;
};

struct AbsolutePosition {
  YType* type = nullptr;
  uint64_t index = 0;
  int64_t assoc = 0;
};

// yjs createRelativePositionFromTypeIndex.
[[nodiscard]] RelativePosition createRelativePosition(const TypeRef& type, uint64_t index,
                                                      int64_t assoc = 0);

// yjs createAbsolutePositionFromRelativePosition; nullopt when the anchor is
// unknown to this doc (missing structs) or was garbage collected.
[[nodiscard]] std::optional<AbsolutePosition> resolveRelativePosition(const RelativePosition& rpos,
                                                                      Doc& doc);

// yjs writeRelativePosition / readRelativePosition (v1 binary form).
[[nodiscard]] std::vector<uint8_t> encodeRelativePosition(const RelativePosition& rpos);
[[nodiscard]] RelativePosition decodeRelativePosition(std::span<const uint8_t> bytes);

}  // namespace ycpp
