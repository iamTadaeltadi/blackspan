#ifndef BLACKSPAN_BYTEIO_H
#define BLACKSPAN_BYTEIO_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace blackspan {

class ByteReader {
 public:
  ByteReader(const uint8_t* data, size_t len)
      : data_(data), len_(len), off_(0), ok_(true) {}

  bool ok() const { return ok_; }
  size_t offset() const { return off_; }
  size_t remaining() const { return off_ <= len_ ? len_ - off_ : 0; }

  uint8_t u8();
  uint16_t u16();
  uint32_t u32();
  uint64_t u64();

  bool read_bytes(uint8_t* out, size_t n);

  const uint8_t* take(size_t n);

  void skip(size_t n);

 private:
  const uint8_t* data_;
  size_t len_;
  size_t off_;
  bool ok_;
};

class ByteWriter {
 public:
  void u8(uint8_t v);
  void u16(uint16_t v);
  void u32(uint32_t v);
  void u64(uint64_t v);
  void bytes(const uint8_t* p, size_t n);
  void bytes(const std::vector<uint8_t>& v) { bytes(v.data(), v.size()); }

  std::vector<uint8_t>& buf() { return buf_; }
  const std::vector<uint8_t>& buf() const { return buf_; }
  size_t size() const { return buf_.size(); }

 private:
  std::vector<uint8_t> buf_;
};

}

#endif
