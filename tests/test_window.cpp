#include <cmath>
#include <cstdio>
#include <vector>

#include "blackspan/window.h"

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

static void test_window_peak() {
  std::vector<double> v = {1.0, -4.0, 2.5, -0.5};
  SampleWindow w;
  w.data = v.data();
  w.len = v.size();
  CHECK(std::fabs(window_peak(w) - 4.0) < 1e-9);

  SampleWindow empty;
  CHECK(window_peak(empty) == 0.0);
}

int main() {
  test_window_peak();
  return failures ? 1 : 0;
}
