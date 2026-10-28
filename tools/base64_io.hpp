// Self-contained base64 codec (RFC 4648, standard alphabet, padded).
//
// Encode: one pass, 3 input bytes become a 24-bit word; each 12-bit half is
// looked up in a 4096-entry table that yields two output characters.
// Decode: one pass over the input with a 256-entry table that classifies each
// byte as sextet, whitespace (skipped), padding or invalid. Clean groups of 4
// take a branch-free fast path; whitespace, padding and errors fall through
// to a per-character path that resumes the fast path at the next group.
//
// Decoding is strict: '=' is accepted only as the last one or two characters
// and the character count without whitespace must be a multiple of 4.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace ycpp_tools {

namespace base64_detail {

inline constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// kPairs[v] is the two characters encoding the 12-bit value v.
inline constexpr std::array<std::array<char, 2>, 4096> kPairs = [] {
  std::array<std::array<char, 2>, 4096> t{};
  for (int v = 0; v < 4096; v++) {
    t[v][0] = kAlphabet[v >> 6];
    t[v][1] = kAlphabet[v & 0x3F];
  }
  return t;
}();

// Decode table classes. Sextets are 0..63; every other class has bit 6 set,
// so `(a | b | c | d) & 0xC0` tests four characters for "all sextets" at once.
inline constexpr uint8_t kSkip = 64;  // whitespace
inline constexpr uint8_t kPad = 65;   // '='
inline constexpr uint8_t kBad = 66;   // anything else

inline constexpr std::array<uint8_t, 256> kDecode = [] {
  std::array<uint8_t, 256> t{};
  for (auto& e : t) e = kBad;
  for (uint8_t i = 0; i < 64; i++) t[static_cast<uint8_t>(kAlphabet[i])] = i;
  t[static_cast<uint8_t>(' ')] = kSkip;
  t[static_cast<uint8_t>('\t')] = kSkip;
  t[static_cast<uint8_t>('\n')] = kSkip;
  t[static_cast<uint8_t>('\r')] = kSkip;
  t[static_cast<uint8_t>('=')] = kPad;
  return t;
}();

}  // namespace base64_detail

inline std::string base64Encode(std::span<const uint8_t> data) {
  using base64_detail::kAlphabet;
  using base64_detail::kPairs;

  const size_t n = data.size();
  std::string out(4 * ((n + 2) / 3), '\0');
  char* p = out.data();
  const uint8_t* s = data.data();

  size_t i = 0;
  for (; i + 3 <= n; i += 3, s += 3, p += 4) {
    const uint32_t w = (uint32_t(s[0]) << 16) | (uint32_t(s[1]) << 8) | s[2];
    const auto& hi = kPairs[w >> 12];
    const auto& lo = kPairs[w & 0xFFF];
    p[0] = hi[0];
    p[1] = hi[1];
    p[2] = lo[0];
    p[3] = lo[1];
  }
  const size_t rem = n - i;
  if (rem == 1) {
    const auto& hi = kPairs[uint32_t(s[0]) << 4];
    p[0] = hi[0];
    p[1] = hi[1];
    p[2] = '=';
    p[3] = '=';
  } else if (rem == 2) {
    const uint32_t w = (uint32_t(s[0]) << 16) | (uint32_t(s[1]) << 8);
    const auto& hi = kPairs[w >> 12];
    p[0] = hi[0];
    p[1] = hi[1];
    p[2] = kAlphabet[(w >> 6) & 0x3F];
    p[3] = '=';
  }
  return out;
}

inline std::vector<uint8_t> base64Decode(const std::string& in) {
  using base64_detail::kBad;
  using base64_detail::kDecode;
  using base64_detail::kPad;
  using base64_detail::kSkip;

  const auto* s = reinterpret_cast<const unsigned char*>(in.data());
  const size_t n = in.size();
  // Upper bound: 3 bytes per full group of 4 characters plus a partial tail.
  std::vector<uint8_t> out(n / 4 * 3 + 3);
  uint8_t* o = out.data();

  size_t i = 0;
  size_t chars = 0;     // characters consumed, whitespace excluded
  uint32_t acc = 0;     // pending sextets of the current group
  unsigned sextets = 0; // number of sextets in acc
  unsigned pad = 0;     // '=' seen so far

  while (i < n) {
    if (sextets == 0 && pad == 0) {
      // Fast path: whole groups of four sextet characters.
      while (i + 4 <= n) {
        const uint32_t a = kDecode[s[i]], b = kDecode[s[i + 1]],
                       c = kDecode[s[i + 2]], d = kDecode[s[i + 3]];
        if ((a | b | c | d) & 0xC0) break;
        const uint32_t w = (a << 18) | (b << 12) | (c << 6) | d;
        o[0] = uint8_t(w >> 16);
        o[1] = uint8_t(w >> 8);
        o[2] = uint8_t(w);
        o += 3;
        i += 4;
        chars += 4;
      }
      if (i >= n) break;
    }
    const uint8_t v = kDecode[s[i++]];
    if (v < 64) {
      if (pad) throw std::runtime_error("base64: data after padding");
      acc = (acc << 6) | v;
      chars++;
      if (++sextets == 4) {
        o[0] = uint8_t(acc >> 16);
        o[1] = uint8_t(acc >> 8);
        o[2] = uint8_t(acc);
        o += 3;
        acc = 0;
        sextets = 0;
      }
    } else if (v == kSkip) {
      continue;
    } else if (v == kPad) {
      if (++pad > 2) throw std::runtime_error("base64: too much padding");
      chars++;
    } else {
      throw std::runtime_error("base64: invalid character");
    }
  }

  if (chars % 4 != 0) throw std::runtime_error("base64: length is not a multiple of 4");
  // chars % 4 == 0 and padding only at the end imply sextets + pad is 0 or 4.
  if (pad == 1) {  // 3 sextets, 18 bits, 2 bytes
    o[0] = uint8_t(acc >> 10);
    o[1] = uint8_t(acc >> 2);
    o += 2;
  } else if (pad == 2) {  // 2 sextets, 12 bits, 1 byte
    o[0] = uint8_t(acc >> 4);
    o += 1;
  }
  out.resize(static_cast<size_t>(o - out.data()));
  return out;
}

}  // namespace ycpp_tools
