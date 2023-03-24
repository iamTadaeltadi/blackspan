#ifndef BLACKSPAN_ARENA_H
#define BLACKSPAN_ARENA_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace blackspan {

class Arena {
 public:
  explicit Arena(size_t block_size = 4096);
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) = delete;
  Arena& operator=(Arena&&) = delete;

  void* allocate(size_t bytes, size_t align = alignof(std::max_align_t));

  uint8_t* dup_bytes(const uint8_t* src, size_t n);

  void reset();

  size_t bytes_used() const { return used_total_; }
  size_t block_count() const { return blocks_.size(); }

 private:
  struct Block {
    uint8_t* data = nullptr;
    size_t cap = 0;
    size_t used = 0;
  };

  void add_block(size_t min_bytes);

  std::vector<Block> blocks_;
  size_t block_size_;
  size_t used_total_ = 0;
};

}

#endif
