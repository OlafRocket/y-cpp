// lib0-compatible binary encoding primitives (subset used by the Yjs v1
// update format): unsigned LEB128 var-ints and length-prefixed UTF-8 strings.
#pragma once

#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace ycpp::lib0 {

class Encoder {
 public:
  void writeUint8(uint8_t v) { buf_.push_back(v); }

  // lib0 encoding.writeVarUint: 7 bits per byte, low bits first,
  // 0x80 marks continuation.
  void writeVarUint(uint64_t num) {
    while (num > 0x7f) {
      buf_.push_back(static_cast<uint8_t>(0x80 | (num & 0x7f)));
      num >>= 7;
    }
    buf_.push_back(static_cast<uint8_t>(num));
  }

  // lib0 encoding.writeVarString: varUint byte length + UTF-8 bytes.
  void writeVarString(const std::string& utf8) {
    writeVarUint(utf8.size());
    buf_.insert(buf_.end(), utf8.begin(), utf8.end());
  }

  void writeBytes(const uint8_t* data, size_t len) {
    buf_.insert(buf_.end(), data, data + len);
  }

  const std::vector<uint8_t>& bytes() const { return buf_; }
  std::vector<uint8_t> take() { return std::move(buf_); }

 private:
  std::vector<uint8_t> buf_;
};

class Decoder {
 public:
  Decoder(const uint8_t* data, size_t len) : data_(data), len_(len) {}
  explicit Decoder(const std::vector<uint8_t>& v) : Decoder(v.data(), v.size()) {}

  bool hasContent() const { return pos_ < len_; }

  uint8_t readUint8() {
    need(1);
    return data_[pos_++];
  }

  uint64_t readVarUint() {
    uint64_t num = 0;
    int shift = 0;
    while (true) {
      uint8_t b = readUint8();
      num |= static_cast<uint64_t>(b & 0x7f) << shift;
      if ((b & 0x80) == 0) return num;
      shift += 7;
      if (shift > 63) throw std::runtime_error("lib0: varUint overflow");
    }
  }

  std::string readVarString() {
    uint64_t n = readVarUint();
    need(n);
    std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return s;
  }

  size_t position() const { return pos_; }

 private:
  void need(uint64_t n) const {
    if (pos_ + n > len_) throw std::runtime_error("lib0: unexpected end of input");
  }
  const uint8_t* data_;
  size_t len_;
  size_t pos_ = 0;
};

}  // namespace ycpp::lib0
