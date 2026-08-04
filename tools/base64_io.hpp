// Thin adapters over the vendored macaron::Base64 codec (third_party/).
// macaron works on std::string and expects clean, padded input; these helpers
// convert byte vectors, tolerate whitespace (trailing newlines from stdin or
// copy/paste), and turn errors into exceptions.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "../third_party/macaron/Base64.h"

namespace ycpp_tools {

inline std::string base64Encode(const std::vector<uint8_t>& data) {
  return macaron::Base64::Encode(std::string(data.begin(), data.end()));
}

inline std::vector<uint8_t> base64Decode(const std::string& in) {
  std::string clean;
  clean.reserve(in.size());
  for (char c : in) {
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
    if (!valid) throw std::runtime_error("base64: invalid character");
    clean.push_back(c);
  }
  if (clean.empty()) return {};
  std::string out;
  std::string err = macaron::Base64::Decode(clean, out);
  if (!err.empty()) throw std::runtime_error("base64: " + err);
  return std::vector<uint8_t>(out.begin(), out.end());
}

}  // namespace ycpp_tools
