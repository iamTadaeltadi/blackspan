#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/pipeline.h"
#include "blackspan/resample.h"

using namespace blackspan;

static int failures = 0;
#define CHECK(cond)                                             \
  do {                                                          \
    if (!(cond)) {                                              \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, \
                   __FILE__, __LINE__);                         \
      ++failures;                                               \
    }                                                           \
  } while (0)

static bool nearly(double a, double b) { return std::fabs(a - b) < 1e-9; }

using Series = std::vector<std::pair<uint64_t, double>>;

// Builds a plan from an explicit grid.
static ResamplePlan plan_of(ResampleMethod m, uint64_t start, uint64_t step,
                            size_t count, uint64_t max_gap = 0) {
  ResamplePlan p;
  p.method = m;
  p.grid.start = start;
  p.grid.step = step;
  p.grid.count = count;
  p.max_gap_ns = max_gap;
  return p;
}

// Two resampled series match: same shape, tallies, timestamps, and values
// (treating NaN as equal to NaN).
static bool series_equal(const ResampledSeries& a, const ResampledSeries& b) {
  if (a.ts.size() != b.ts.size()) return false;
  if (a.values.size() != b.values.size()) return false;
  if (a.defined != b.defined || a.nan != b.nan) return false;
  for (size_t i = 0; i < a.ts.size(); ++i) {
    if (a.ts[i] != b.ts[i]) return false;
    const bool an = std::isnan(a.values[i]);
    const bool bn = std::isnan(b.values[i]);
    if (an != bn) return false;
    if (!an && !nearly(a.values[i], b.values[i])) return false;
  }
  return true;
}

static void test_names_roundtrip() {
  const ResampleMethod ms[] = {ResampleMethod::Nearest,
                               ResampleMethod::PreviousHold,
                               ResampleMethod::Linear};
  for (ResampleMethod m : ms) {
    ResampleMethod parsed;
    CHECK(parse_resample_method(resample_method_name(m), parsed));
    CHECK(parsed == m);
  }
  ResampleMethod dummy;
  CHECK(!parse_resample_method("cubic", dummy));
  CHECK(!parse_resample_method("", dummy));
}

static void test_previous_hold() {
  // Rising step series; PreviousHold holds the last sample at or before each
  // grid point. Values are exact.
  Series s = {{0, 0}, {10, 10}, {20, 30}};
  auto r = resample_series(s, plan_of(ResampleMethod::PreviousHold, 0, 5, 5));
  // Grid points: 0, 5, 10, 15, 20.
  CHECK(r.values.size() == 5 && r.ts.size() == 5);
  CHECK(r.ts[0] == 0 && r.ts[4] == 20);
  CHECK(r.values[0] == 0.0);   // exactly at ts 0
  CHECK(r.values[1] == 0.0);   // 5 -> hold ts 0
  CHECK(r.values[2] == 10.0);  // exactly at ts 10
  CHECK(r.values[3] == 10.0);  // 15 -> hold ts 10
  CHECK(r.values[4] == 30.0);  // exactly at ts 20
  CHECK(r.defined == 5 && r.nan == 0);
}

static void test_nearest() {
  // Nearest picks the closer bracketing sample; ties go to the earlier one.
  Series s = {{0, 0}, {10, 10}, {20, 30}};
  auto r = resample_series(s, plan_of(ResampleMethod::Nearest, 0, 1, 21));
  // Grid points 0..20 by 1.
  CHECK(r.values[0] == 0.0);    // ts 0
  CHECK(r.values[5] == 0.0);    // 5: tie between 0 and 10 -> earlier (0)
  CHECK(r.values[6] == 10.0);   // 6: closer to 10
  CHECK(r.values[10] == 10.0);  // ts 10
  CHECK(r.values[14] == 10.0);  // 14: closer to 10
  CHECK(r.values[15] == 10.0);  // 15: tie between 10 and 20 -> earlier (10)
  CHECK(r.values[16] == 30.0);  // 16: closer to 20
  CHECK(r.values[20] == 30.0);  // ts 20
  CHECK(r.defined == 21 && r.nan == 0);
}

static void test_linear() {
  // Linear interpolates by timestamp fraction between bracketing samples.
  Series s = {{0, 0}, {10, 100}, {20, 300}};
  auto r = resample_series(s, plan_of(ResampleMethod::Linear, 0, 1, 21));
  CHECK(nearly(r.values[0], 0.0));     // ts 0
  CHECK(nearly(r.values[2], 20.0));    // 0 + (100-0)*0.2
  CHECK(nearly(r.values[5], 50.0));    // midpoint of [0,10]
  CHECK(nearly(r.values[10], 100.0));  // exactly at ts 10
  CHECK(nearly(r.values[15], 200.0));  // midpoint of [10,20]: 100 + 200*0.5
  CHECK(nearly(r.values[18], 260.0));  // 100 + 200*0.8
  CHECK(nearly(r.values[20], 300.0));  // ts 20
  CHECK(r.defined == 21 && r.nan == 0);
}

static void test_grid_from_series() {
  Series s = {{100, 1}, {150, 2}, {200, 3}, {250, 4}};
  ResampleGrid g = grid_from_series(s, 50);
  CHECK(g.start == 100);
  CHECK(g.step == 50);
  CHECK(g.count == 4);  // 100,150,200,250
  CHECK(g.at(0) == 100 && g.at(3) == 250);

  // A step that does not divide the span evenly: last point is <= last ts.
  Series u = {{0, 0}, {100, 1}};
  ResampleGrid g2 = grid_from_series(u, 30);
  CHECK(g2.start == 0 && g2.step == 30);
  CHECK(g2.count == 4);  // 0,30,60,90 (90 <= 100)
  CHECK(g2.at(3) == 90);

  // Degenerate inputs yield a zero-count grid.
  CHECK(grid_from_series({}, 50).count == 0);
  CHECK(grid_from_series(s, 0).count == 0);

  // A single-sample series spans one point.
  Series one = {{42, 7}};
  ResampleGrid g3 = grid_from_series(one, 10);
  CHECK(g3.start == 42 && g3.count == 1);
}

static void test_max_gap_hold() {
  // A short lead-in then a long gap to a far sample.
  Series s = {{0, 0}, {10, 10}, {1000, 1000}};
  // Grid 0,100,...,1000 (11 points).
  auto r = resample_series(s, plan_of(ResampleMethod::PreviousHold, 0, 100, 11,
                                      /*max_gap=*/100));
  CHECK(r.values.size() == 11);
  CHECK(r.values[0] == 0.0);           // gap 0
  CHECK(r.values[1] == 10.0);          // ts 100, holds ts 10 (gap 90 <= 100)
  CHECK(std::isnan(r.values[2]));      // ts 200, gap 190 > 100
  CHECK(std::isnan(r.values[9]));      // ts 900, far from ts 10
  CHECK(r.values[10] == 1000.0);       // exactly at ts 1000
  CHECK(r.defined == 3 && r.nan == 8);
}

static void test_max_gap_linear() {
  Series s = {{0, 0}, {10, 10}, {1000, 1000}};
  // Wide brackets are refused: points 0, 500, 1000.
  auto wide = resample_series(
      s, plan_of(ResampleMethod::Linear, 0, 500, 3, /*max_gap=*/100));
  CHECK(wide.values[0] == 0.0);       // exact hit, no bridging
  CHECK(std::isnan(wide.values[1]));  // bracket [10,1000] width 990 > 100
  CHECK(wide.values[2] == 1000.0);    // exact hit at last sample
  CHECK(wide.defined == 2 && wide.nan == 1);

  // A small bracket is honored: points 0, 5, 10.
  auto small = resample_series(
      s, plan_of(ResampleMethod::Linear, 0, 5, 3, /*max_gap=*/100));
  CHECK(nearly(small.values[0], 0.0));
  CHECK(nearly(small.values[1], 5.0));  // bracket [0,10] width 10 <= 100
  CHECK(nearly(small.values[2], 10.0));
  CHECK(small.defined == 3 && small.nan == 0);
}

static void test_outside_range() {
  // Grid points before the first and after the last sample are NaN.
  Series s = {{10, 1}, {20, 2}, {30, 3}};
  auto r = resample_series(s, plan_of(ResampleMethod::Nearest, 0, 10, 5));
  // Grid 0,10,20,30,40.
  CHECK(std::isnan(r.values[0]));  // 0 < first ts 10
  CHECK(r.values[1] == 1.0);
  CHECK(r.values[2] == 2.0);
  CHECK(r.values[3] == 3.0);
  CHECK(std::isnan(r.values[4]));  // 40 > last ts 30
  CHECK(r.defined == 3 && r.nan == 2);
}

static void test_ties_last_value() {
  // Two samples share ts 10 (values 5 then 9); the tie resolves to the last.
  Series s = {{0, 1}, {10, 5}, {10, 9}, {20, 3}};
  auto hold = resample_series(s, plan_of(ResampleMethod::PreviousHold, 0, 5, 5));
  // Grid 0,5,10,15,20.
  CHECK(hold.values[0] == 1.0);
  CHECK(hold.values[1] == 1.0);  // hold ts 0
  CHECK(hold.values[2] == 9.0);  // ts 10 -> last sample there (9)
  CHECK(hold.values[3] == 9.0);  // hold ts 10 -> 9
  CHECK(hold.values[4] == 3.0);  // ts 20

  auto near = resample_series(s, plan_of(ResampleMethod::Nearest, 10, 1, 1));
  CHECK(near.values[0] == 9.0);  // exact hit, last sample at ts 10

  auto lin = resample_series(s, plan_of(ResampleMethod::Linear, 10, 1, 1));
  CHECK(lin.values[0] == 9.0);  // exact hit resolves to last value
}

static void test_empty_input() {
  // Empty input yields the grid with every value NaN.
  auto r = resample_series({}, plan_of(ResampleMethod::Linear, 0, 10, 4));
  CHECK(r.ts.size() == 4 && r.values.size() == 4);
  CHECK(r.ts[0] == 0 && r.ts[3] == 30);
  CHECK(r.defined == 0 && r.nan == 4);
  for (double v : r.values) CHECK(std::isnan(v));
}

static void test_single_sample() {
  Series s = {{50, 7}};
  // Grid 0,25,50,75,100; only ts 50 is inside [50,50].
  for (ResampleMethod m : {ResampleMethod::Nearest, ResampleMethod::PreviousHold,
                           ResampleMethod::Linear}) {
    auto r = resample_series(s, plan_of(m, 0, 25, 5));
    CHECK(std::isnan(r.values[0]));  // 0 < 50
    CHECK(std::isnan(r.values[1]));  // 25 < 50
    CHECK(r.values[2] == 7.0);       // exactly at the only sample
    CHECK(std::isnan(r.values[3]));  // 75 > 50
    CHECK(std::isnan(r.values[4]));  // 100 > 50
    CHECK(r.defined == 1 && r.nan == 4);
  }

  // With a gap bound, the single sample is still defined at its own timestamp
  // (distance 0) and NaN elsewhere.
  auto g = resample_series(
      s, plan_of(ResampleMethod::PreviousHold, 0, 25, 5, /*max_gap=*/5));
  CHECK(g.values[2] == 7.0);
  CHECK(g.defined == 1 && g.nan == 4);
}

static void test_unsorted_matches_sorted() {
  Series sorted = {{0, 0}, {10, 20}, {25, 5}, {40, 80}, {55, 15}};
  Series shuffled = {{40, 80}, {0, 0}, {55, 15}, {10, 20}, {25, 5}};

  ResampleGrid grid = grid_from_series(sorted, 5);
  ResamplePlan p;
  p.method = ResampleMethod::Linear;
  p.grid = grid;

  auto a = resample_series(sorted, p);
  auto b = resample_series(shuffled, p);
  CHECK(a.ts.size() == grid.count);
  CHECK(series_equal(a, b));

  // Also compare under Nearest and PreviousHold.
  p.method = ResampleMethod::Nearest;
  CHECK(series_equal(resample_series(sorted, p), resample_series(shuffled, p)));
  p.method = ResampleMethod::PreviousHold;
  CHECK(series_equal(resample_series(sorted, p), resample_series(shuffled, p)));
}

// Builds a two-block recording for one channel and confirms the ChannelStore
// adapter matches the core run over the same (block_ts, value) pairs.
static void test_adapter() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"  // base ts 1000
      "  row 10\n"
      "  row 20\n"
      "end\n"
      "samples channel=1 delta=0 ts=1100\n"  // base ts 1100
      "  row 30\n"
      "  row 40\n"
      "end\n"
      "session-end reason=0\n";
  AssembleResult a = assemble_script(script);
  for (const AssembleDiagnostic& d : a.errors) {
    std::fprintf(stderr, "assemble: %s\n",
                 describe_assemble_diagnostic(d).c_str());
  }
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());

  // The pairs the adapter should reconstruct: each sample inherits its block ts.
  Series pairs = {{1000, 10}, {1000, 20}, {1100, 30}, {1100, 40}};
  ResamplePlan p = plan_of(ResampleMethod::PreviousHold, 1000, 50, 3);

  ResampledSeries expected = resample_series(pairs, p);
  ResampledSeries by_index = resample_channel(rec.channels(), 1, size_t{0}, p);
  ResampledSeries by_name = resample_channel(rec.channels(), 1, "v", p);

  CHECK(series_equal(by_index, expected));
  CHECK(series_equal(by_name, expected));

  // Concrete spot checks: hold resolves ts-1000 tie to the last value (20).
  CHECK(by_index.ts.size() == 3);
  CHECK(by_index.values[0] == 20.0);  // ts 1000 -> last sample there
  CHECK(by_index.values[1] == 20.0);  // ts 1050 -> hold ts 1000
  CHECK(by_index.values[2] == 40.0);  // ts 1100 -> last sample there

  // Unknown channel / field: grid honored, all NaN.
  auto bad_ch = resample_channel(rec.channels(), 99, size_t{0}, p);
  CHECK(bad_ch.ts.size() == 3 && bad_ch.defined == 0 && bad_ch.nan == 3);
  auto bad_field_idx = resample_channel(rec.channels(), 1, size_t{5}, p);
  CHECK(bad_field_idx.defined == 0 && bad_field_idx.nan == 3);
  auto bad_field_name = resample_channel(rec.channels(), 1, "nope", p);
  CHECK(bad_field_name.defined == 0 && bad_field_name.nan == 3);
}

int main() {
  test_names_roundtrip();
  test_previous_hold();
  test_nearest();
  test_linear();
  test_grid_from_series();
  test_max_gap_hold();
  test_max_gap_linear();
  test_outside_range();
  test_ties_last_value();
  test_empty_input();
  test_single_sample();
  test_unsorted_matches_sorted();
  test_adapter();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_resample: all checks passed\n");
  return 0;
}
