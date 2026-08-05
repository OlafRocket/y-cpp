// Public handles over shared type nodes: TypeRef and the Text / Array /
// Map APIs. Cheap to copy; they borrow the node, the Doc owns it.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "ycpp/event.hpp"
#include "ycpp/type.hpp"

namespace ycpp {
class Text;
class Array;
class Map;
// ------------------------------------------------------------------ wrappers

class TypeRef {
 public:
  TypeRef(YType* t) : t_(t) {}
  uint64_t length() const { return t_->length; }
  YType* node() const { return t_; }

  // Observers fire at the end of the transaction that changed this type
  // (local edits and applyUpdate alike). Returns a handle for unobserve().
  uint64_t observe(Observer fn);
  void unobserve(uint64_t handle);

  // RAII variant of observe(): the observer detaches when the returned
  // Subscription is destroyed or reset().
  Subscription subscribe(Observer fn);

 protected:
  YType* t_;
};

// Y.Text: indices and lengths in UTF-16 code units, matching JavaScript.
class Text : public TypeRef {
 public:
  using TypeRef::TypeRef;
  void insert(uint64_t index, const std::string& utf8);
  void insertUtf16(uint64_t index, const std::u16string& s);
  void remove(uint64_t index, uint64_t delLen);
  [[nodiscard]] std::string toString() const;  // UTF-8
  [[nodiscard]] std::u16string toUtf16() const;
};

class Array : public TypeRef {
 public:
  using TypeRef::TypeRef;
  void insert(uint64_t index, const std::vector<Any>& values);
  void push(const Any& value) { insert(length(), {value}); }
  void remove(uint64_t index, uint64_t delLen);
  Value get(uint64_t index) const;
  Array insertArray(uint64_t index);
  Map insertMap(uint64_t index);
  Text insertText(uint64_t index);
  std::string toJson() const;
};

class Map : public TypeRef {
 public:
  using TypeRef::TypeRef;
  void set(const std::string& key, const Any& value);
  void remove(const std::string& key);
  bool has(const std::string& key) const;
  std::optional<Value> get(const std::string& key) const;
  std::vector<std::string> keys() const;
  size_t size() const;
  Array setArray(const std::string& key);
  Map setMap(const std::string& key);
  Text setText(const std::string& key);
  std::string toJson() const;
};

}  // namespace ycpp
