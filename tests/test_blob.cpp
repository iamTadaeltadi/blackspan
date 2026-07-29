#include <cstdint>
#include <cstdio>
#include <vector>

#include "blackspan/blob.h"

using namespace blackspan;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, \
                   __LINE__);                                             \
      ++failures;                                                         \
    }                                                                     \
  } while (0)

static void test_owned_buffer_move_and_checksum() {
  const uint8_t bytes[] = {1, 2, 3, 4};
  OwnedBuffer a(bytes, 4);
  CHECK(a.size() == 4);
  const uint32_t cs = a.checksum();
  OwnedBuffer b(std::move(a));
  CHECK(b.size() == 4);
  CHECK(b.checksum() == cs);
  CHECK(a.size() == 0);
}

static void test_index_accumulates() {
  BlobIndex idx;
  const uint8_t bytes[] = {9, 8, 7};
  for (int i = 0; i < 10; ++i) idx.add(bytes, 3);
  CHECK(idx.size() == 10);
  CHECK(idx.hot() == 0);
  CHECK(idx.total_bytes() == 30);
}

int main() {
  test_owned_buffer_move_and_checksum();
  test_index_accumulates();
  return failures ? 1 : 0;
}
