// The shared type node (yjs AbstractType): list start + key map, and the
// wire ids of the shared-type kinds.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "ycpp/event.hpp"

namespace ycpp {
class Doc;
struct Item;
// Wire ids of shared-type kinds (yjs writeTypeRef).
enum class TypeKind : uint8_t {
  Array = 0, Map = 1, Text = 2,
  XmlElement = 3, XmlFragment = 4, XmlHook = 5, XmlText = 6,
  Unknown = 255,  // root referenced by an update before any local getX() call
};

// A shared type node (yjs AbstractType): a linked list of items (_start) for
// sequence content and a key -> item map (_map) for map content.
struct YType {
  TypeKind kind = TypeKind::Unknown;
  std::string rootName;                // set for root types
  std::string xmlName;                 // node/hook name for XmlElement/XmlHook
  Item* item = nullptr;                // the ContentType item (null for roots)
  Item* start = nullptr;
  std::map<std::string, Item*> map;    // parentSub -> most recent item
  uint64_t length = 0;                 // countable list length (UTF-16 for text)
  Doc* doc = nullptr;
  std::vector<std::pair<uint64_t, Observer>> observers;
};

}  // namespace ycpp
