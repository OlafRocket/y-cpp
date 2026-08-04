// UTF-8 <-> UTF-16 conversion with JavaScript semantics: invalid input and
// unpaired surrogates become U+FFFD, matching what TextEncoder/TextDecoder do
// in the reference yjs implementation. Yjs clock lengths are UTF-16 units.
#pragma once

#include <cstdint>
#include <string>

namespace ycpp::utf {

inline std::u16string utf8To16(const std::string& in) {
  std::u16string out;
  out.reserve(in.size());
  size_t i = 0;
  const size_t n = in.size();
  while (i < n) {
    uint8_t c = static_cast<uint8_t>(in[i]);
    uint32_t cp = 0xFFFD;
    size_t len = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < n && (in[i + 1] & 0xC0) == 0x80) {
      cp = ((c & 0x1F) << 6) | (in[i + 1] & 0x3F);
      len = 2;
      if (cp < 0x80) cp = 0xFFFD;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < n && (in[i + 1] & 0xC0) == 0x80 &&
               (in[i + 2] & 0xC0) == 0x80) {
      cp = ((c & 0x0F) << 12) | ((in[i + 1] & 0x3F) << 6) | (in[i + 2] & 0x3F);
      len = 3;
      if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < n && (in[i + 1] & 0xC0) == 0x80 &&
               (in[i + 2] & 0xC0) == 0x80 && (in[i + 3] & 0xC0) == 0x80) {
      cp = ((c & 0x07) << 18) | ((in[i + 1] & 0x3F) << 12) |
           ((in[i + 2] & 0x3F) << 6) | (in[i + 3] & 0x3F);
      len = 4;
      if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
    }
    i += len;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 | (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 | (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
  }
  return out;
}

inline void appendCp(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

inline std::string utf16To8(const std::u16string& in) {
  std::string out;
  out.reserve(in.size() * 3);
  const size_t n = in.size();
  for (size_t i = 0; i < n; i++) {
    char16_t u = in[i];
    if (u >= 0xD800 && u <= 0xDBFF) {
      if (i + 1 < n && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
        uint32_t cp = 0x10000 + ((static_cast<uint32_t>(u) - 0xD800) << 10) +
                      (static_cast<uint32_t>(in[i + 1]) - 0xDC00);
        appendCp(out, cp);
        i++;
      } else {
        appendCp(out, 0xFFFD);  // unpaired high surrogate
      }
    } else if (u >= 0xDC00 && u <= 0xDFFF) {
      appendCp(out, 0xFFFD);  // unpaired low surrogate
    } else {
      appendCp(out, u);
    }
  }
  return out;
}

}  // namespace ycpp::utf
