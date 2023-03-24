#include <cstdint>
#include <cstdio>
#include <string>

#include "blackspan/arena.h"
#include "blackspan/pool.h"

using namespace blackspan;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond,         \
                   __FILE__, __LINE__);                                 \
      ++failures;                                                       \
    }                                                                   \
  } while (0)

static void test_pool_basic() {
  Pool<int> p;
  auto a = p.alloc(10);
  auto b = p.alloc(20);
  auto c = p.alloc(30);
  CHECK(p.live_count() == 3);
  CHECK(p.get(a) && *p.get(a) == 10);
  CHECK(p.get(b) && *p.get(b) == 20);
  CHECK(p.get(c) && *p.get(c) == 30);

  CHECK(p.free(b));
  CHECK(p.live_count() == 2);
  CHECK(p.get(b) == nullptr);
  CHECK(!p.free(b));

  auto d = p.alloc(40);
  CHECK(d.index == b.index);
  CHECK(d.gen != b.gen);
  CHECK(p.get(b) == nullptr);
  CHECK(p.get(d) && *p.get(d) == 40);
  CHECK(p.live_count() == 3);
}

static void test_pool_null_handle() {
  Pool<int> p;
  Pool<int>::Handle null_handle;
  CHECK(!null_handle.valid());
  CHECK(p.get(null_handle) == nullptr);
  CHECK(!p.free(null_handle));
}

static void test_pool_destructs_values() {

  Pool<std::string> p;
  auto h1 = p.alloc(std::string(64, 'x'));
  auto h2 = p.alloc("hello");
  CHECK(p.get(h1) && p.get(h1)->size() == 64);
  CHECK(p.get(h2) && *p.get(h2) == "hello");
  CHECK(p.free(h1));
  p.clear();
  CHECK(p.live_count() == 0);
  CHECK(p.get(h2) == nullptr);
}

static void test_arena() {
  Arena a(128);
  void* p1 = a.allocate(10, 8);
  void* p2 = a.allocate(10, 8);
  CHECK(p1 != p2);
  CHECK((reinterpret_cast<uintptr_t>(p1) % 8) == 0);
  CHECK((reinterpret_cast<uintptr_t>(p2) % 8) == 0);
  CHECK(a.bytes_used() == 20);

  void* big = a.allocate(4096, 16);
  CHECK(big != nullptr);
  CHECK((reinterpret_cast<uintptr_t>(big) % 16) == 0);

  const uint8_t src[4] = {1, 2, 3, 4};
  uint8_t* copy = a.dup_bytes(src, 4);
  CHECK(copy[0] == 1 && copy[3] == 4);
  CHECK(copy != src);

  a.reset();
  CHECK(a.bytes_used() == 0);
  CHECK(a.block_count() == 1);
  void* after = a.allocate(8, 8);
  CHECK(after != nullptr);
}

int main() {
  test_pool_basic();
  test_pool_null_handle();
  test_pool_destructs_values();
  test_arena();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all common tests passed\n");
  return 0;
}
