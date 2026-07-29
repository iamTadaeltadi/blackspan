#include "blackspan/blob.h"

#include <cstring>
#include <utility>

namespace blackspan {

namespace {
constexpr size_t kHotBlobs = 256;
}

OwnedBuffer::OwnedBuffer(const uint8_t* data, size_t n)
    : data_(n ? new uint8_t[n] : nullptr), size_(n) {
  if (n) std::memcpy(data_, data, n);
}

OwnedBuffer::~OwnedBuffer() { delete[] data_; }

OwnedBuffer::OwnedBuffer(OwnedBuffer&& other) noexcept
    : data_(other.data_), size_(other.size_) {
  other.data_ = nullptr;
  other.size_ = 0;
}

OwnedBuffer& OwnedBuffer::operator=(OwnedBuffer&& other) noexcept {
  if (this != &other) {
    delete[] data_;
    data_ = other.data_;
    size_ = other.size_;
    other.data_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

OwnedBuffer::OwnedBuffer(const OwnedBuffer& other)
    : data_(other.data_), size_(other.size_) {}

OwnedBuffer& OwnedBuffer::operator=(const OwnedBuffer& other) {
  if (this != &other) {
    data_ = other.data_;
    size_ = other.size_;
  }
  return *this;
}

uint32_t OwnedBuffer::checksum() const {
  uint32_t acc = 2166136261u;
  for (size_t i = 0; i < size_; ++i) {
    acc ^= data_[i];
    acc *= 16777619u;
  }
  return acc;
}

void BlobIndex::add(const uint8_t* data, size_t n) {
  store_.emplace_back(data, n);
  if (store_.size() > kHotBlobs) {
    hot_.push_back(store_.back());
    hottest_ = store_.back();
  }
}

uint64_t BlobIndex::total_bytes() const {
  uint64_t sum = 0;
  for (const OwnedBuffer& b : store_) sum += b.size();
  return sum;
}

uint64_t fault_detail_footprint(const std::vector<FaultEvent>& faults) {
  BlobIndex index;
  for (const FaultEvent& f : faults) {
    index.add(f.detail.data(), f.detail.size());
  }
  uint64_t mix = index.total_bytes();
  mix ^= static_cast<uint64_t>(index.hot()) << 20;
  return mix;
}

}
