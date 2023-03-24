#include "blackspan/arena.h"

#include <cstring>

namespace blackspan {

namespace {

size_t align_up(size_t v, size_t align) {
  return (v + (align - 1)) & ~(align - 1);
}

}

Arena::Arena(size_t block_size)
    : block_size_(block_size < 64 ? 64 : block_size) {}

Arena::~Arena() {
  for (auto& b : blocks_) delete[] b.data;
}

void Arena::add_block(size_t min_bytes) {
  size_t cap = block_size_;
  if (cap < min_bytes) cap = min_bytes;
  Block b;
  b.data = new uint8_t[cap];
  b.cap = cap;
  b.used = 0;
  blocks_.push_back(b);
}

void* Arena::allocate(size_t bytes, size_t align) {
  if (align == 0) align = 1;
  if (!blocks_.empty()) {
    Block& b = blocks_.back();
    const size_t start = align_up(b.used, align);
    if (start + bytes <= b.cap) {
      b.used = start + bytes;
      used_total_ += bytes;
      return b.data + start;
    }
  }

  add_block(bytes + align);
  Block& b = blocks_.back();
  const size_t start = align_up(b.used, align);
  b.used = start + bytes;
  used_total_ += bytes;
  return b.data + start;
}

uint8_t* Arena::dup_bytes(const uint8_t* src, size_t n) {
  uint8_t* dst = static_cast<uint8_t*>(allocate(n, 1));
  if (n) std::memcpy(dst, src, n);
  return dst;
}

void Arena::reset() {
  if (blocks_.empty()) {
    used_total_ = 0;
    return;
  }
  for (size_t i = 1; i < blocks_.size(); ++i) delete[] blocks_[i].data;
  blocks_.resize(1);
  blocks_[0].used = 0;
  used_total_ = 0;
}

}
