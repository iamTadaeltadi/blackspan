#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/health.h"
#include "blackspan/pipeline.h"

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

static bool near(double a, double b, double eps = 1e-6) {
  return std::fabs(a - b) <= eps;
}

// A constant column has a baseline equal to every sample, so its volatility is 0.
static void test_constant_is_quiet() {
  std::vector<double> col(8, 7.0);
  CHECK(near(volatility_score(col), 0.0));
}

// Too-short columns score 0 (no baseline to deviate from).
static void test_degenerate() {
  CHECK(near(volatility_score({}), 0.0));
  CHECK(near(volatility_score({42.0}), 0.0));
}

// An eight-sample alternating column has a half-window of one; the mean absolute
// deviation from the three-point moving average works out to 6.25 (hand-checked:
// the two endpoints deviate by 5, the six interior points by 20/3 each).
static void test_alternating_score() {
  std::vector<double> col = {0, 10, 0, 10, 0, 10, 0, 10};
  CHECK(near(volatility_score(col), 6.25, 1e-3));
}

// The scan visits every field of every channel and flags those above threshold.
static void test_scan_over_store() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"
      "  row 0\n"
      "  row 10\n"
      "  row 0\n"
      "  row 10\n"
      "  row 0\n"
      "  row 10\n"
      "  row 0\n"
      "  row 10\n"
      "end\n"
      "session-end reason=0\n";
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());

  std::vector<HealthFinding> findings = channel_health_scan(rec.channels(), 1.0);
  CHECK(findings.size() == 1);
  if (!findings.empty()) {
    const HealthFinding& f = findings[0];
    CHECK(f.channel_id == 1);
    CHECK(f.field_index == 0);
    CHECK(f.field_name == "v");
    CHECK(f.samples == 8);
    CHECK(near(f.score, 6.25, 1e-3));
    CHECK(f.flagged);  // 6.25 > 1.0
    CHECK(!describe_health(f).empty());
  }

  // A high threshold leaves the same field unflagged.
  std::vector<HealthFinding> quiet = channel_health_scan(rec.channels(), 100.0);
  CHECK(quiet.size() == 1);
  if (!quiet.empty()) CHECK(!quiet[0].flagged);
}

int main() {
  test_constant_is_quiet();
  test_degenerate();
  test_alternating_score();
  test_scan_over_store();
  return failures ? 1 : 0;
}
