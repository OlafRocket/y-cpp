// "Any" values: the JSON-ish data model lib0 encodes for Y.Map / Y.Array
// entries (lib0 encoding.writeAny / readAny), plus JSON serialization.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "ycpp/lib0.hpp"

namespace ycpp {

struct Any {
  enum class Kind : uint8_t {
    Undefined, Null, Bool, Int, BigInt, Double, String, Binary, List, Dict
  };

  Kind kind = Kind::Undefined;
  bool boolean = false;
  int64_t integer = 0;  // Int and BigInt
  double number = 0;
  std::string str;  // UTF-8
  std::vector<uint8_t> bin;
  std::vector<Any> list;
  std::map<std::string, Any> dict;

  Any() = default;
  static Any null() { Any a; a.kind = Kind::Null; return a; }
  static Any of(bool v) { Any a; a.kind = Kind::Bool; a.boolean = v; return a; }
  static Any of(int64_t v) { Any a; a.kind = Kind::Int; a.integer = v; return a; }
  static Any of(int v) { return of(static_cast<int64_t>(v)); }
  static Any of(double v) { Any a; a.kind = Kind::Double; a.number = v; return a; }
  static Any of(const std::string& v) { Any a; a.kind = Kind::String; a.str = v; return a; }
  static Any of(const char* v) { return of(std::string(v)); }
  static Any binary(std::vector<uint8_t> v) { Any a; a.kind = Kind::Binary; a.bin = std::move(v); return a; }
  static Any array(std::vector<Any> v) { Any a; a.kind = Kind::List; a.list = std::move(v); return a; }
  static Any object(std::map<std::string, Any> v) { Any a; a.kind = Kind::Dict; a.dict = std::move(v); return a; }

  bool operator==(const Any& o) const {
    if (kind != o.kind) return false;
    switch (kind) {
      case Kind::Undefined: case Kind::Null: return true;
      case Kind::Bool: return boolean == o.boolean;
      case Kind::Int: case Kind::BigInt: return integer == o.integer;
      case Kind::Double: return number == o.number;
      case Kind::String: return str == o.str;
      case Kind::Binary: return bin == o.bin;
      case Kind::List: return list == o.list;
      case Kind::Dict: return dict == o.dict;
    }
    return false;
  }
};

namespace anycodec {

// lib0 writeAny. Mirrors the JS type choices so common values byte-match.
inline void write(lib0::Encoder& enc, const Any& a) {
  switch (a.kind) {
    case Any::Kind::Undefined: enc.writeUint8(127); break;
    case Any::Kind::Null: enc.writeUint8(126); break;
    case Any::Kind::Bool: enc.writeUint8(a.boolean ? 120 : 121); break;
    case Any::Kind::Int:
      if (a.integer >= -0x7fffffffLL && a.integer <= 0x7fffffffLL) {
        enc.writeUint8(125);
        enc.writeVarInt(a.integer);
      } else {  // JS encodes unsafe-as-int numbers as float64
        enc.writeUint8(123);
        enc.writeFloat64BE(static_cast<double>(a.integer));
      }
      break;
    case Any::Kind::BigInt:
      enc.writeUint8(122);
      enc.writeInt64BE(a.integer);
      break;
    case Any::Kind::Double:
      enc.writeUint8(123);
      enc.writeFloat64BE(a.number);
      break;
    case Any::Kind::String:
      enc.writeUint8(119);
      enc.writeVarString(a.str);
      break;
    case Any::Kind::Binary:
      enc.writeUint8(116);
      enc.writeVarUint(a.bin.size());
      enc.writeBytes(a.bin.data(), a.bin.size());
      break;
    case Any::Kind::List:
      enc.writeUint8(117);
      enc.writeVarUint(a.list.size());
      for (const Any& v : a.list) write(enc, v);
      break;
    case Any::Kind::Dict:
      enc.writeUint8(118);
      enc.writeVarUint(a.dict.size());
      for (const auto& [k, v] : a.dict) {
        enc.writeVarString(k);
        write(enc, v);
      }
      break;
  }
}

inline Any read(lib0::Decoder& dec) {
  Any a;
  uint8_t tag = dec.readUint8();
  switch (tag) {
    case 127: a.kind = Any::Kind::Undefined; break;
    case 126: a.kind = Any::Kind::Null; break;
    case 125: a.kind = Any::Kind::Int; a.integer = dec.readVarInt(); break;
    case 124: a.kind = Any::Kind::Double; a.number = dec.readFloat32BE(); break;
    case 123: a.kind = Any::Kind::Double; a.number = dec.readFloat64BE(); break;
    case 122: a.kind = Any::Kind::BigInt; a.integer = dec.readInt64BE(); break;
    case 121: a.kind = Any::Kind::Bool; a.boolean = false; break;
    case 120: a.kind = Any::Kind::Bool; a.boolean = true; break;
    case 119: a.kind = Any::Kind::String; a.str = dec.readVarString(); break;
    case 118: {
      a.kind = Any::Kind::Dict;
      uint64_t n = dec.readVarUint();
      for (uint64_t i = 0; i < n; i++) {
        std::string key = dec.readVarString();
        a.dict[key] = read(dec);
      }
      break;
    }
    case 117: {
      a.kind = Any::Kind::List;
      uint64_t n = dec.readVarUint();
      a.list.reserve(n);
      for (uint64_t i = 0; i < n; i++) a.list.push_back(read(dec));
      break;
    }
    case 116: {
      a.kind = Any::Kind::Binary;
      uint64_t n = dec.readVarUint();
      a.bin = dec.readBytes(n);
      break;
    }
    default:
      throw std::runtime_error("lib0: unknown Any tag " + std::to_string(tag));
  }
  return a;
}

}  // namespace anycodec

namespace json {

inline void escape(std::string& out, const std::string& s) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

// JSON text for an Any. undefined becomes null; binary becomes a tagged
// string ("base64:<...>" is written by callers that can encode base64 —
// here it is a byte count placeholder to keep this header dependency-free).
inline void dump(std::string& out, const Any& a) {
  char buf[32];
  switch (a.kind) {
    case Any::Kind::Undefined: case Any::Kind::Null: out += "null"; break;
    case Any::Kind::Bool: out += a.boolean ? "true" : "false"; break;
    case Any::Kind::Int: case Any::Kind::BigInt:
      std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(a.integer));
      out += buf;
      break;
    case Any::Kind::Double:
      if (std::isfinite(a.number)) {
        std::snprintf(buf, sizeof buf, "%.17g", a.number);
        out += buf;
      } else {
        out += "null";
      }
      break;
    case Any::Kind::String: escape(out, a.str); break;
    case Any::Kind::Binary:
      std::snprintf(buf, sizeof buf, "\"<%zu bytes>\"", a.bin.size());
      out += buf;
      break;
    case Any::Kind::List: {
      out.push_back('[');
      bool first = true;
      for (const Any& v : a.list) {
        if (!first) out.push_back(',');
        first = false;
        dump(out, v);
      }
      out.push_back(']');
      break;
    }
    case Any::Kind::Dict: {
      out.push_back('{');
      bool first = true;
      for (const auto& [k, v] : a.dict) {
        if (!first) out.push_back(',');
        first = false;
        escape(out, k);
        out.push_back(':');
        dump(out, v);
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace json

}  // namespace ycpp
