#include "blackspan/byteio.h"

namespace blackspan {

uint8_t ByteReader::u8() {
  if (off_ + 1 > len_) {
    ok_ = false;
    off_ = len_;
    return 0;
  }
  return data_[off_++];
}

uint16_t ByteReader::u16() {
  if (off_ + 2 > len_) {
    ok_ = false;
    off_ = len_;
    return 0;
  }
  uint16_t v = static_cast<uint16_t>(data_[off_]) |
               static_cast<uint16_t>(data_[off_ + 1] << 8);
  off_ += 2;
  return v;
}

uint32_t ByteReader::u32() {
  if (off_ + 4 > len_) {
    ok_ = false;
    off_ = len_;
    return 0;
  }
  uint32_t v = static_cast<uint32_t>(data_[off_]) |
               (static_cast<uint32_t>(data_[off_ + 1]) << 8) |
               (static_cast<uint32_t>(data_[off_ + 2]) << 16) |
               (static_cast<uint32_t>(data_[off_ + 3]) << 24);
  off_ += 4;
  return v;
}

uint64_t ByteReader::u64() {
  if (off_ + 8 > len_) {
    ok_ = false;
    off_ = len_;
    return 0;
  }
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(data_[off_ + i]) << (8 * i);
  }
  off_ += 8;
  return v;
}

bool ByteReader::read_bytes(uint8_t* out, size_t n) {
  if (off_ + n > len_) {
    ok_ = false;
    off_ = len_;
    return false;
  }
  for (size_t i = 0; i < n; ++i) out[i] = data_[off_ + i];
  off_ += n;
  return true;
}

const uint8_t* ByteReader::take(size_t n) {
  if (off_ + n > len_) {
    ok_ = false;
    return nullptr;
  }
  const uint8_t* p = data_ + off_;
  off_ += n;
  return p;
}

void ByteReader::skip(size_t n) {
  if (off_ + n > len_) {
    ok_ = false;
    off_ = len_;
    return;
  }
  off_ += n;
}

void ByteWriter::u8(uint8_t v) { buf_.push_back(v); }

void ByteWriter::u16(uint16_t v) {
  buf_.push_back(static_cast<uint8_t>(v & 0xFF));
  buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void ByteWriter::u32(uint32_t v) {
  for (int i = 0; i < 4; ++i)
    buf_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void ByteWriter::u64(uint64_t v) {
  for (int i = 0; i < 8; ++i)
    buf_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void ByteWriter::bytes(const uint8_t* p, size_t n) {
  buf_.insert(buf_.end(), p, p + n);
}

}  // namespace blackspan
