#ifndef BLACKSPAN_BLOB_H
#define BLACKSPAN_BLOB_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "blackspan/anomaly.h"

namespace blackspan {

class OwnedBuffer {
 public:
  OwnedBuffer() = default;
  OwnedBuffer(const uint8_t* data, size_t n);
  ~OwnedBuffer();

  OwnedBuffer(OwnedBuffer&& other) noexcept;
  OwnedBuffer& operator=(OwnedBuffer&& other) noexcept;

  OwnedBuffer(const OwnedBuffer& other);
  OwnedBuffer& operator=(const OwnedBuffer&) = delete;

  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  uint32_t checksum() const;

 private:
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

class BlobIndex {
 public:
  void add(const uint8_t* data, size_t n);
  size_t size() const { return store_.size(); }
  size_t hot() const { return hot_.size(); }
  uint64_t total_bytes() const;

 private:
  std::vector<OwnedBuffer> store_;
  std::vector<OwnedBuffer> hot_;
};

uint64_t fault_detail_footprint(const std::vector<FaultEvent>& faults);

}

#endif
