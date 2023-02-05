#ifndef BLACKSPAN_ARENA_H
#define BLACKSPAN_ARENA_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace blackspan {

// A block-based bump allocator for short-lived scratch data (for example the
// intermediate buffers used while evaluating a derived channel). Individual
// allocations are never freed; the whole arena is reset or destroyed at once.
// It is intended for trivially destructible data and raw byte spans.
//
// Non-copyable and non-movable: it owns its blocks and is held in place.
class Arena {
 public:
  explicit Arena(size_t block_size = 4096);
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) = delete;
  Arena& operator=(Arena&&) = delete;

  // Returns a pointer to `bytes` bytes aligned to `align`. Never null; grows by
  // adding a block large enough for the request.
  void* allocate(size_t bytes, size_t align = alignof(std::max_align_t));

  // Copies n bytes into arena-owned storage and returns the copy.
  uint8_t* dup_bytes(const uint8_t* src, size_t n);

  // Frees every block except a single retained first block and rewinds it.
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

}  // namespace blackspan

#endif  // BLACKSPAN_ARENA_H
