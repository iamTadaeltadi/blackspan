#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/aggregate.h"
#include "blackspan/assemble.h"
#include "blackspan/pipeline.h"

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

static void test_names_roundtrip() {
  const AggFn fns[] = {AggFn::Mean, AggFn::Min,   AggFn::Max, AggFn::First,
                       AggFn::Last, AggFn::Count, AggFn::Sum};
  for (AggFn f : fns) {
    AggFn parsed;
    CHECK(parse_agg_fn(agg_fn_name(f), parsed));
    CHECK(parsed == f);
  }
  AggFn dummy;
  CHECK(!parse_agg_fn("median", dummy));
}

static void test_aggregate_series_windows() {

  std::vector<std::pair<uint64_t, double>> s = {
      {0, 1}, {10, 2}, {20, 3}, {30, 4}, {40, 5}, {50, 6}};

  auto mean = aggregate_series(s, 30, AggFn::Mean);
  CHECK(mean.size() == 2);
  if (mean.size() == 2) {
    CHECK(mean[0].window_start == 0);
    CHECK(mean[0].count == 3);
    CHECK(nearly(mean[0].value, 2.0));
    CHECK(mean[1].window_start == 30);
    CHECK(nearly(mean[1].value, 5.0));
  }

  auto mx = aggregate_series(s, 30, AggFn::Max);
  CHECK(mx.size() == 2);
  if (mx.size() == 2) {
    CHECK(nearly(mx[0].value, 3.0));
    CHECK(nearly(mx[1].value, 6.0));
  }

  auto mn = aggregate_series(s, 30, AggFn::Min);
  if (mn.size() == 2) {
    CHECK(nearly(mn[0].value, 1.0));
    CHECK(nearly(mn[1].value, 4.0));
  }

  auto first = aggregate_series(s, 30, AggFn::First);
  auto last = aggregate_series(s, 30, AggFn::Last);
  if (first.size() == 2 && last.size() == 2) {
    CHECK(nearly(first[0].value, 1.0));
    CHECK(nearly(last[0].value, 3.0));
    CHECK(nearly(first[1].value, 4.0));
    CHECK(nearly(last[1].value, 6.0));
  }

  auto count = aggregate_series(s, 30, AggFn::Count);
  if (count.size() == 2) {
    CHECK(nearly(count[0].value, 3.0));
    CHECK(nearly(count[1].value, 3.0));
  }

  auto sum = aggregate_series(s, 30, AggFn::Sum);
  if (sum.size() == 2) {
    CHECK(nearly(sum[0].value, 6.0));
    CHECK(nearly(sum[1].value, 15.0));
  }
}

static void test_sparse_windows_skip_empty() {

  std::vector<std::pair<uint64_t, double>> s = {{0, 1}, {100, 2}};
  auto buckets = aggregate_series(s, 30, AggFn::Mean);
  CHECK(buckets.size() == 2);
  if (buckets.size() == 2) {
    CHECK(buckets[0].window_start == 0);
    CHECK(buckets[1].window_start == 90);
  }
}

static void test_unsorted_input() {
  std::vector<std::pair<uint64_t, double>> s = {
      {40, 5}, {0, 1}, {20, 3}, {10, 2}, {50, 6}, {30, 4}};
  auto buckets = aggregate_series(s, 30, AggFn::First);
  CHECK(buckets.size() == 2);
  if (buckets.size() == 2) {

    CHECK(nearly(buckets[0].value, 1.0));
    CHECK(nearly(buckets[1].value, 4.0));
  }
}

static void test_degenerate() {
  std::vector<std::pair<uint64_t, double>> s = {{0, 1}, {10, 2}};
  CHECK(aggregate_series(s, 0, AggFn::Mean).empty());
  CHECK(aggregate_series({}, 30, AggFn::Mean).empty());

  auto one = aggregate_series(s, 1000, AggFn::Sum);
  CHECK(one.size() == 1);
  if (one.size() == 1) CHECK(nearly(one[0].value, 3.0));
}

static void test_from_recording() {

  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"
      "  row 10\n"
      "  row 20\n"
      "end\n"
      "samples channel=1 delta=0 ts=1100\n"
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

  auto pairs = column_with_timestamps(rec.channels(), 1, 0);
  CHECK(pairs.size() == 4);
  if (pairs.size() == 4) {
    CHECK(pairs[0].first == 1000 && nearly(pairs[0].second, 10));
    CHECK(pairs[1].first == 1000 && nearly(pairs[1].second, 20));
    CHECK(pairs[2].first == 1100 && nearly(pairs[2].second, 30));
    CHECK(pairs[3].first == 1100 && nearly(pairs[3].second, 40));
  }

  auto buckets = aggregate_channel(rec.channels(), 1, 0, 100, AggFn::Mean);
  CHECK(buckets.size() == 2);
  if (buckets.size() == 2) {
    CHECK(nearly(buckets[0].value, 15.0));
    CHECK(nearly(buckets[1].value, 35.0));
  }

  CHECK(column_with_timestamps(rec.channels(), 99, 0).empty());
  CHECK(column_with_timestamps(rec.channels(), 1, 5).empty());
}

static void test_render() {
  std::vector<std::pair<uint64_t, double>> s = {{0, 1}, {10, 2}};
  auto buckets = aggregate_series(s, 30, AggFn::Mean);
  std::string a = render_buckets(buckets, AggFn::Mean);
  std::string b = render_buckets(buckets, AggFn::Mean);
  CHECK(a == b);
  CHECK(a.find("windows (mean)") != std::string::npos);
  CHECK(render_buckets({}, AggFn::Mean) == "0 windows\n");
}

int main() {
  test_names_roundtrip();
  test_aggregate_series_windows();
  test_sparse_windows_skip_empty();
  test_unsorted_input();
  test_degenerate();
  test_from_recording();
  test_render();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_aggregate: all checks passed\n");
  return 0;
}
