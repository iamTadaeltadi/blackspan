#ifndef BLACKSPAN_POOL_H
#define BLACKSPAN_POOL_H

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace blackspan {

// A stable-handle object pool. Handles carry a generation so that a handle to a
// freed (and possibly reused) slot is rejected rather than silently aliasing a
// different object. Objects live at stable heap addresses; only the small slot
// table grows, so a handle's target pointer never moves for the object's life.
//
// The pool is non-copyable and non-movable: it owns heap objects and is meant
// to be held in place as a subsystem member.
template <typename T>
class Pool {
 public:
  struct Handle {
    uint32_t index = 0;
    uint32_t gen = 0;  // 0 == null handle
    bool valid() const { return gen != 0; }
    bool operator==(const Handle& o) const {
      return index == o.index && gen == o.gen;
    }
  };

  Pool() = default;
  ~Pool() { clear(); }
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;
  Pool(Pool&&) = delete;
  Pool& operator=(Pool&&) = delete;

  Handle alloc(T value) {
    uint32_t idx;
    if (!free_list_.empty()) {
      idx = free_list_.back();
      free_list_.pop_back();
    } else {
      idx = static_cast<uint32_t>(slots_.size());
      slots_.push_back(Slot{});
    }
    Slot& s = slots_[idx];
    s.ptr = new T(std::move(value));
    s.alive = true;
    if (s.gen == 0) s.gen = 1;
    ++live_;
    return Handle{idx, s.gen};
  }

  bool free(Handle h) {
    if (!valid(h)) return false;
    Slot& s = slots_[h.index];
    delete s.ptr;
    s.ptr = nullptr;
    s.alive = false;
    ++s.gen;
    if (s.gen == 0) s.gen = 1;  // never reuse the null generation
    free_list_.push_back(h.index);
    --live_;
    return true;
  }

  T* get(Handle h) { return valid(h) ? slots_[h.index].ptr : nullptr; }
  const T* get(Handle h) const {
    return valid(h) ? slots_[h.index].ptr : nullptr;
  }

  bool alive(Handle h) const { return valid(h); }
  size_t live_count() const { return live_; }
  size_t capacity() const { return slots_.size(); }

  void clear() {
    for (auto& s : slots_) {
      if (s.ptr) {
        delete s.ptr;
        s.ptr = nullptr;
        s.alive = false;
      }
    }
    slots_.clear();
    free_list_.clear();
    live_ = 0;
  }

 private:
  struct Slot {
    T* ptr = nullptr;
    uint32_t gen = 0;
    bool alive = false;
  };

  bool valid(Handle h) const {
    return h.gen != 0 && h.index < slots_.size() && slots_[h.index].alive &&
           slots_[h.index].gen == h.gen;
  }

  std::vector<Slot> slots_;
  std::vector<uint32_t> free_list_;
  size_t live_ = 0;
};

}  // namespace blackspan

#endif  // BLACKSPAN_POOL_H
