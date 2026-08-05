// Doc basics: construction, arenas, root types, store lookups, validate.
#include "ycpp/ycpp.hpp"

#include <algorithm>
#include <exception>
#include <random>
#include <stdexcept>
#include <unordered_set>

#include "internal.hpp"

namespace ycpp {

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
      if (!it->deleted && it->kind == ContentKind::String && it->len != it->str().size()) {
        fail("string item length mismatch");
      }
      if (!it->deleted && it->kind == ContentKind::Any && it->len != it->anyVals().size()) {
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

}  // namespace ycpp
