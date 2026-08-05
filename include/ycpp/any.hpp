// "Any" values: the JSON-ish data model lib0 encodes for Y.Map / Y.Array
// entries (lib0 encoding.writeAny / readAny), plus JSON serialization.
//
// Storage is a std::variant, so accessing a value as the wrong kind throws
// std::bad_variant_access instead of silently reading a stale member.
#pragma once

#include <cmath>
#include <cstdint>
#include <concepts>
#include <cstdio>
#include <map>
#include <type_traits>
#include <string>
#include <variant>
#include <vector>

#include "ycpp/lib0.hpp"

namespace ycpp {

class Any {
 public:
  // Distinct from Int: same width, different wire tag (lib0 BigInt64).
  struct BigIntValue {
    int64_t v = 0;
    bool operator==(const BigIntValue&) const = default;
  };
  using Binary = std::vector<uint8_t>;
  using List = std::vector<Any>;
  using Dict = std::map<std::string, Any>;

  // Order matches the variant alternatives: kind() is the variant index.
  enum class Kind : uint8_t {
    Undefined, Null, Bool, Int, BigInt, Double, String, Binary, List, Dict
  };

  Any() = default;
  static Any null() { return Any(nullptr); }
  static Any of(bool v) { return Any(v); }
  static Any of(int64_t v) { return Any(v); }
  static Any of(int v) { return of(static_cast<int64_t>(v)); }
  static Any of(double v) { return Any(v); }
  static Any of(std::string v) { return Any(std::move(v)); }
  static Any of(const char* v) { return of(std::string(v)); }
  static Any bigint(int64_t v) { return Any(BigIntValue{v}); }
  static Any binary(Binary v) { return Any(std::move(v)); }
  static Any array(List v) { return Any(std::move(v)); }
  static Any object(Dict v) { return Any(std::move(v)); }

  [[nodiscard]] Kind kind() const { return static_cast<Kind>(v_.index()); }
  [[nodiscard]] bool boolean() const { return std::get<bool>(v_); }
  [[nodiscard]] int64_t integer() const {
    if (auto* b = std::get_if<BigIntValue>(&v_)) return b->v;
    return std::get<int64_t>(v_);
  }
  [[nodiscard]] double number() const { return std::get<double>(v_); }
  [[nodiscard]] const std::string& str() const { return std::get<std::string>(v_); }
  [[nodiscard]] const Binary& bin() const { return std::get<Binary>(v_); }
  [[nodiscard]] const List& list() const { return std::get<List>(v_); }
  [[nodiscard]] const Dict& dict() const { return std::get<Dict>(v_); }

  bool operator==(const Any&) const = default;

 private:
  using Storage = std::variant<std::monostate, std::nullptr_t, bool, int64_t,
                               BigIntValue, double, std::string, Binary, List, Dict>;
  // Constrained so this never hijacks Any's own copy/move constructors.
  template <typename T>
    requires(!std::same_as<std::decay_t<T>, Any> && std::is_constructible_v<Storage, T&&>)
  explicit Any(T&& v) : v_(std::forward<T>(v)) {}

  Storage v_;
};

namespace anycodec {

// lib0 writeAny. Mirrors the JS type choices so common values byte-match.
inline void write(lib0::Encoder& enc, const Any& a) {
  using Kind = Any::Kind;
  switch (a.kind()) {
    case Kind::Undefined: enc.writeUint8(127); break;
    case Kind::Null: enc.writeUint8(126); break;
    case Kind::Bool: enc.writeUint8(a.boolean() ? 120 : 121); break;
    case Kind::Int:
      if (a.integer() >= -0x7fffffffLL && a.integer() <= 0x7fffffffLL) {
        enc.writeUint8(125);
        enc.writeVarInt(a.integer());
      } else {  // JS encodes unsafe-as-int numbers as float64
        enc.writeUint8(123);
        enc.writeFloat64BE(static_cast<double>(a.integer()));
      }
      break;
    case Kind::BigInt:
      enc.writeUint8(122);
      enc.writeInt64BE(a.integer());
      break;
    case Kind::Double:
      enc.writeUint8(123);
      enc.writeFloat64BE(a.number());
      break;
    case Kind::String:
      enc.writeUint8(119);
      enc.writeVarString(a.str());
      break;
    case Kind::Binary:
      enc.writeUint8(116);
      enc.writeVarUint(a.bin().size());
      enc.writeBytes(a.bin());
      break;
    case Kind::List:
      enc.writeUint8(117);
      enc.writeVarUint(a.list().size());
      for (const Any& v : a.list()) write(enc, v);
      break;
    case Kind::Dict:
      enc.writeUint8(118);
      enc.writeVarUint(a.dict().size());
      for (const auto& [k, v] : a.dict()) {
        enc.writeVarString(k);
        write(enc, v);
      }
      break;
  }
}

inline Any read(lib0::Decoder& dec) {
  uint8_t tag = dec.readUint8();
  switch (tag) {
    case 127: return Any();
    case 126: return Any::null();
    case 125: return Any::of(dec.readVarInt());
    case 124: return Any::of(dec.readFloat32BE());
    case 123: return Any::of(dec.readFloat64BE());
    case 122: return Any::bigint(dec.readInt64BE());
    case 121: return Any::of(false);
    case 120: return Any::of(true);
    case 119: return Any::of(dec.readVarString());
    case 118: {
      Any::Dict d;
      uint64_t n = dec.readVarUint();
      for (uint64_t i = 0; i < n; i++) {
        std::string key = dec.readVarString();
        d.emplace(std::move(key), read(dec));
      }
      return Any::object(std::move(d));
    }
    case 117: {
      Any::List l;
      uint64_t n = dec.readVarUint();
      l.reserve(n);
      for (uint64_t i = 0; i < n; i++) l.push_back(read(dec));
      return Any::array(std::move(l));
    }
    case 116: {
      uint64_t n = dec.readVarUint();
      return Any::binary(dec.readBytes(n));
    }
    default:
      throw std::runtime_error("lib0: unknown Any tag " + std::to_string(tag));
  }
}

}  // namespace anycodec

}  // namespace ycpp
