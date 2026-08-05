// List/map operations, the Text/Array/Map wrappers, JSON dump.
#include "ycpp/ycpp.hpp"

#include <algorithm>
#include <exception>
#include <random>
#include <stdexcept>
#include <unordered_set>

#include "internal.hpp"

namespace ycpp {

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
  it->content = s;
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
    if (!it->deleted && it->kind == ContentKind::String) out += it->str();
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
  it->content = values;
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
  YType* nested = d->allocType(TypeKind::Array);
  nested->item = it;
  it->content = nested;
  it->len = 1;
  d->listInsert(t_, index, it);
  return Array(nested);
}

Map Array::insertMap(uint64_t index) {
  Doc* d = t_->doc;
  Item* it = d->alloc();
  it->kind = ContentKind::Type;
  YType* nested = d->allocType(TypeKind::Map);
  nested->item = it;
  it->content = nested;
  it->len = 1;
  d->listInsert(t_, index, it);
  return Map(nested);
}

Text Array::insertText(uint64_t index) {
  Doc* d = t_->doc;
  Item* it = d->alloc();
  it->kind = ContentKind::Type;
  YType* nested = d->allocType(TypeKind::Text);
  nested->item = it;
  it->content = nested;
  it->len = 1;
  d->listInsert(t_, index, it);
  return Text(nested);
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
  it->content = std::vector<Any>{value};
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
  YType* nested = d->allocType(TypeKind::Array);
  nested->item = it;
  it->content = nested;
  it->len = 1;
  d->mapSet(t_, key, it);
  return Array(nested);
}

Map Map::setMap(const std::string& key) {
  Doc* d = t_->doc;
  Item* it = d->alloc();
  it->kind = ContentKind::Type;
  YType* nested = d->allocType(TypeKind::Map);
  nested->item = it;
  it->content = nested;
  it->len = 1;
  d->mapSet(t_, key, it);
  return Map(nested);
}

Text Map::setText(const std::string& key) {
  Doc* d = t_->doc;
  Item* it = d->alloc();
  it->kind = ContentKind::Type;
  YType* nested = d->allocType(TypeKind::Text);
  nested->item = it;
  it->content = nested;
  it->len = 1;
  d->mapSet(t_, key, it);
  return Text(nested);
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
