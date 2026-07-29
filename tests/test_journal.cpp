#include <cmath>
#include <cstdio>

#include "blackspan/journal.h"

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

static void test_stage_and_flush() {
  AnnotationJournal j;
  j.reset(4);
  j.stage(0, 1.0f);
  j.stage(2, 3.0f);
  j.stage(3, -2.0f);
  CHECK(j.pending() == 3);
  CHECK(j.flush() == 3);
  CHECK(j.pending() == 0);
  CHECK(std::fabs(j.table()[0] - 1.0f) < 1e-6f);
  CHECK(std::fabs(j.table()[2] - 3.0f) < 1e-6f);
  CHECK(std::fabs(j.table()[3] + 2.0f) < 1e-6f);
}

static void test_compact_reduces_extent() {
  AnnotationJournal j;
  j.reset(16);
  const size_t removed = j.compact();
  CHECK(removed == 2);
  CHECK(j.extent() == 14);
}

int main() {
  test_stage_and_flush();
  test_compact_reduces_extent();
  return failures ? 1 : 0;
}
