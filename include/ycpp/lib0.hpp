// lib0-compatible binary encoding primitives (subset used by the Yjs v1
// update format): unsigned LEB128 var-ints and length-prefixed UTF-8 strings.
#pragma once

#include <bit>
#include <cstdint>
#include <cstddef>
#include <span>
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

  // lib0 encoding.writeVarInt: signed var-int, sign bit 0x40 in the first
  // byte, 6 payload bits first byte then 7 per continuation byte.
  void writeVarInt(int64_t num) {
    bool negative = num < 0;
    uint64_t n = negative ? static_cast<uint64_t>(-num) : static_cast<uint64_t>(num);
    buf_.push_back(static_cast<uint8_t>((n > 0x3f ? 0x80 : 0) |
                                        (negative ? 0x40 : 0) | (n & 0x3f)));
    n >>= 6;
    while (n > 0) {
      buf_.push_back(static_cast<uint8_t>((n > 0x7f ? 0x80 : 0) | (n & 0x7f)));
      n >>= 7;
    }
  }

  // lib0 encoding.writeVarString: varUint byte length + UTF-8 bytes.
  void writeVarString(const std::string& utf8) {
    writeVarUint(utf8.size());
    buf_.insert(buf_.end(), utf8.begin(), utf8.end());
  }

  void writeFloat64BE(double v) {
    uint64_t bits = std::bit_cast<uint64_t>(v);
    for (int i = 7; i >= 0; i--) buf_.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xff));
  }

  void writeInt64BE(int64_t v) {
    uint64_t bits = static_cast<uint64_t>(v);
    for (int i = 7; i >= 0; i--) buf_.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xff));
  }

  void writeBytes(std::span<const uint8_t> data) {
    buf_.insert(buf_.end(), data.begin(), data.end());
  }

  // lib0 encoding.writeVarUint8Array: varUint byte length + bytes.
  void writeVarUint8Array(std::span<const uint8_t> data) {
    writeVarUint(data.size());
    writeBytes(data);
  }

  const std::vector<uint8_t>& bytes() const { return buf_; }
  std::vector<uint8_t> take() { return std::move(buf_); }

 private:
  std::vector<uint8_t> buf_;
};

class Decoder {
 public:
  explicit Decoder(std::span<const uint8_t> data) : data_(data.data()), len_(data.size()) {}

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

  int64_t readVarInt() {
    uint8_t b = readUint8();
    bool negative = (b & 0x40) != 0;
    uint64_t n = b & 0x3f;
    int shift = 6;
    while (b & 0x80) {
      b = readUint8();
      n |= static_cast<uint64_t>(b & 0x7f) << shift;
      shift += 7;
      if (shift > 63) throw std::runtime_error("lib0: varInt overflow");
    }
    return negative ? -static_cast<int64_t>(n) : static_cast<int64_t>(n);
  }

  std::string readVarString() {
    uint64_t n = readVarUint();
    need(n);
    std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return s;
  }

  // lib0 decoding.readVarUint8Array: varUint byte length + bytes.
  std::vector<uint8_t> readVarUint8Array() { return readBytes(readVarUint()); }

  std::vector<uint8_t> readBytes(uint64_t n) {
    need(n);
    std::vector<uint8_t> out(data_ + pos_, data_ + pos_ + n);
    pos_ += n;
    return out;
  }

  double readFloat64BE() {
    uint64_t bits = 0;
    for (int i = 0; i < 8; i++) bits = (bits << 8) | readUint8();
    return std::bit_cast<double>(bits);
  }

  double readFloat32BE() {
    uint32_t bits = 0;
    for (int i = 0; i < 4; i++) bits = (bits << 8) | readUint8();
    return static_cast<double>(std::bit_cast<float>(bits));
  }

  int64_t readInt64BE() {
    uint64_t bits = 0;
    for (int i = 0; i < 8; i++) bits = (bits << 8) | readUint8();
    return static_cast<int64_t>(bits);
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
