// JSON text output for Any values (diagnostics/inspection; output-only).
#pragma once

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

#include "ycpp/any.hpp"
#include "ycpp/utf.hpp"

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


// Strict JSON parser producing Any values (numbers become Int when integral
// and in int64 range, Double otherwise; null -> Kind::Null). Throws
// std::runtime_error on malformed input or trailing garbage.
inline Any parse(const std::string& text) {
  size_t pos = 0;
  auto fail = [&](const std::string& msg) -> void {
    throw std::runtime_error("json: " + msg + " at offset " + std::to_string(pos));
  };
  auto skipWs = [&] {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' ||
                                 text[pos] == '\n' || text[pos] == '\r')) {
      pos++;
    }
  };
  auto peek = [&]() -> char {
    if (pos >= text.size()) fail("unexpected end of input");
    return text[pos];
  };
  auto expect = [&](char c) {
    if (peek() != c) fail(std::string("expected '") + c + "'");
    pos++;
  };
  auto literal = [&](const char* word) {
    for (const char* p = word; *p; p++) {
      if (pos >= text.size() || text[pos] != *p) fail("invalid literal");
      pos++;
    }
  };

  std::function<Any()> value;

  auto parseString = [&]() -> std::string {
    expect('"');
    std::string out;
    while (true) {
      if (pos >= text.size()) fail("unterminated string");
      unsigned char c = static_cast<unsigned char>(text[pos]);
      if (c == '"') { pos++; break; }
      if (c == '\\') {
        pos++;
        char e = peek();
        pos++;
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            auto hex4 = [&]() -> unsigned {
              unsigned v = 0;
              for (int i = 0; i < 4; i++) {
                char h = peek();
                pos++;
                v <<= 4;
                if (h >= '0' && h <= '9') v |= static_cast<unsigned>(h - '0');
                else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned>(h - 'A' + 10);
                else fail("invalid \\u escape");
              }
              return v;
            };
            unsigned u = hex4();
            char16_t units[2];
            size_t n = 1;
            units[0] = static_cast<char16_t>(u);
            if (u >= 0xD800 && u <= 0xDBFF && pos + 1 < text.size() &&
                text[pos] == '\\' && text[pos + 1] == 'u') {
              pos += 2;
              units[1] = static_cast<char16_t>(hex4());
              n = 2;
            }
            // reuse the UTF-16 -> UTF-8 conversion (handles lone surrogates)
            out += utf::utf16To8(std::u16string(units, n));
            break;
          }
          default: fail("invalid escape");
        }
      } else if (c < 0x20) {
        fail("control character in string");
      } else {
        out.push_back(static_cast<char>(c));
        pos++;
      }
    }
    return out;
  };

  auto parseNumber = [&]() -> Any {
    size_t start = pos;
    if (peek() == '-') pos++;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') pos++;
    bool integral = true;
    if (pos < text.size() && text[pos] == '.') {
      integral = false;
      pos++;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') pos++;
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
      integral = false;
      pos++;
      if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) pos++;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') pos++;
    }
    std::string num = text.substr(start, pos - start);
    if (num.empty() || num == "-") fail("invalid number");
    if (integral) {
      errno = 0;
      char* end = nullptr;
      long long v = std::strtoll(num.c_str(), &end, 10);
      if (errno == 0 && end == num.c_str() + num.size()) return Any::of(static_cast<int64_t>(v));
    }
    return Any::of(std::strtod(num.c_str(), nullptr));
  };

  value = [&]() -> Any {
    skipWs();
    switch (peek()) {
      case 'n': literal("null"); return Any::null();
      case 't': literal("true"); return Any::of(true);
      case 'f': literal("false"); return Any::of(false);
      case '"': return Any::of(parseString());
      case '[': {
        pos++;
        Any::List l;
        skipWs();
        if (peek() == ']') { pos++; return Any::array(std::move(l)); }
        while (true) {
          l.push_back(value());
          skipWs();
          if (peek() == ',') { pos++; continue; }
          expect(']');
          break;
        }
        return Any::array(std::move(l));
      }
      case '{': {
        pos++;
        Any::Dict d;
        skipWs();
        if (peek() == '}') { pos++; return Any::object(std::move(d)); }
        while (true) {
          skipWs();
          std::string key = parseString();
          skipWs();
          expect(':');
          d[key] = value();
          skipWs();
          if (peek() == ',') { pos++; continue; }
          expect('}');
          break;
        }
        return Any::object(std::move(d));
      }
      default: return parseNumber();
    }
  };

  Any result = value();
  skipWs();
  if (pos != text.size()) fail("trailing garbage");
  return result;
}

}  // namespace json

}  // namespace ycpp
