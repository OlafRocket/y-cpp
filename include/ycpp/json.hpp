// JSON text output for Any values (diagnostics/inspection; output-only).
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

#include "ycpp/any.hpp"

namespace ycpp {

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

// JSON text for an Any. undefined becomes null; binary becomes a placeholder
// string (this header stays dependency-free of base64).
inline void dump(std::string& out, const Any& a) {
  using Kind = Any::Kind;
  char buf[32];
  switch (a.kind()) {
    case Kind::Undefined: case Kind::Null: out += "null"; break;
    case Kind::Bool: out += a.boolean() ? "true" : "false"; break;
    case Kind::Int: case Kind::BigInt:
      std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(a.integer()));
      out += buf;
      break;
    case Kind::Double:
      if (std::isfinite(a.number())) {
        std::snprintf(buf, sizeof buf, "%.17g", a.number());
        out += buf;
      } else {
        out += "null";
      }
      break;
    case Kind::String: escape(out, a.str()); break;
    case Kind::Binary:
      std::snprintf(buf, sizeof buf, "\"<%zu bytes>\"", a.bin().size());
      out += buf;
      break;
    case Kind::List: {
      out.push_back('[');
      bool first = true;
      for (const Any& v : a.list()) {
        if (!first) out.push_back(',');
        first = false;
        dump(out, v);
      }
      out.push_back(']');
      break;
    }
    case Kind::Dict: {
      out.push_back('{');
      bool first = true;
      for (const auto& [k, v] : a.dict()) {
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
