#include <cmath>
#include <cstdio>

#include "blackspan/gauge.h"

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

static void test_board_aggregate() {
  GaugeBoard board;
  board.watch(1, 0, 2.0);
  board.watch(1, 1, -3.0);
  board.watch(2, 0, 1.5);
  CHECK(board.size() == 3);
  CHECK(std::fabs(board.aggregate() - 7.5) < 1e-9);
}

int main() {
  test_board_aggregate();
  return failures ? 1 : 0;
}
