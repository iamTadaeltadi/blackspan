#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/pipeline.h"
#include "blackspan/xchannel.h"

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

static bool close_to(double a, double b, double eps) {
  return std::fabs(a - b) < eps;
}

using Series = std::vector<std::pair<uint64_t, double>>;

static bool same_ref(const ChannelFieldRef& r, uint16_t ch, size_t f) {
  return r.channel_id == ch && r.field_index == f;
}

static void test_perfect_positive() {
  Series left = {{10, 1}, {20, 2}, {30, 3}, {40, 4}, {50, 5}};
  Series right = {{10, 2}, {20, 4}, {30, 6}, {40, 8}, {50, 10}};

  PairStats s = pair_stats(left, right, JoinKind::Inner, 0);
  CHECK(s.n == 5);
  CHECK(close_to(s.correlation, 1.0, 1e-9));
  CHECK(close_to(s.slope, 2.0, 1e-9));
  CHECK(close_to(s.intercept, 0.0, 1e-9));
  CHECK(close_to(s.r2, 1.0, 1e-9));
  CHECK(close_to(s.rmse, 0.0, 1e-9));
  CHECK(close_to(s.covariance, 4.0, 1e-9));
  CHECK(!std::isnan(s.correlation));

  CHECK(close_to(aligned_correlation(left, right, JoinKind::Inner, 0), 1.0,
                 1e-9));
}

static void test_perfect_negative() {
  Series left = {{10, 1}, {20, 2}, {30, 3}, {40, 4}, {50, 5}};
  Series right = {{10, 10}, {20, 8}, {30, 6}, {40, 4}, {50, 2}};

  PairStats s = pair_stats(left, right, JoinKind::Inner, 0);
  CHECK(s.n == 5);
  CHECK(close_to(s.correlation, -1.0, 1e-9));
  CHECK(close_to(s.slope, -2.0, 1e-9));
  CHECK(close_to(s.r2, 1.0, 1e-9));
  CHECK(s.covariance < 0.0);
  CHECK(close_to(s.rmse, 0.0, 1e-9));
}

static void test_uncorrelated() {
  Series left = {{10, 1}, {20, 2}, {30, 3}, {40, 4}, {50, 5}};
  Series right = {{10, 1}, {20, 2}, {30, 1}, {40, 2}, {50, 1}};

  PairStats s = pair_stats(left, right, JoinKind::Inner, 0);
  CHECK(s.n == 5);
  CHECK(close_to(s.correlation, 0.0, 1e-9));
  CHECK(std::fabs(s.correlation) < 0.01);
  CHECK(!std::isnan(s.correlation));
}

static void test_zero_variance_left() {
  Series left = {{10, 7}, {20, 7}, {30, 7}, {40, 7}};
  Series right = {{10, 1}, {20, 3}, {30, 5}, {40, 7}};

  PairStats s = pair_stats(left, right, JoinKind::Inner, 0);
  CHECK(s.n == 4);
  CHECK(close_to(s.correlation, 0.0, 1e-12));
  CHECK(close_to(s.slope, 0.0, 1e-12));
  CHECK(close_to(s.r2, 0.0, 1e-12));
  CHECK(close_to(s.intercept, 4.0, 1e-9));
  CHECK(!std::isnan(s.correlation));
  CHECK(!std::isnan(s.slope));
  CHECK(!std::isnan(s.intercept));
  CHECK(!std::isnan(s.r2));
  CHECK(!std::isnan(s.rmse));
  CHECK(!std::isnan(s.covariance));
}

static void test_zero_variance_right() {
  Series left = {{10, 1}, {20, 3}, {30, 5}, {40, 7}};
  Series right = {{10, 9}, {20, 9}, {30, 9}, {40, 9}};

  PairStats s = pair_stats(left, right, JoinKind::Inner, 0);
  CHECK(s.n == 4);
  CHECK(close_to(s.correlation, 0.0, 1e-12));
  CHECK(close_to(s.slope, 0.0, 1e-12));
  CHECK(close_to(s.intercept, 9.0, 1e-9));
  CHECK(close_to(s.rmse, 0.0, 1e-9));
  CHECK(!std::isnan(s.correlation));
}

static void test_aligned_direct() {
  std::vector<double> xs = {1, 2, 3, 4};
  std::vector<double> ys = {3, 5, 7, 9};
  PairStats s = pair_stats_aligned(xs, ys);
  CHECK(s.n == 4);
  CHECK(close_to(s.slope, 2.0, 1e-9));
  CHECK(close_to(s.intercept, 1.0, 1e-9));
  CHECK(close_to(s.correlation, 1.0, 1e-9));

  PairStats single = pair_stats_aligned({5.0}, {8.0});
  CHECK(single.n == 1);
  CHECK(close_to(single.correlation, 0.0, 1e-12));
  CHECK(close_to(single.slope, 0.0, 1e-12));
  CHECK(close_to(single.intercept, 8.0, 1e-9));
  CHECK(close_to(single.rmse, 0.0, 1e-12));

  PairStats none = pair_stats_aligned({}, {});
  CHECK(none.n == 0);
  CHECK(close_to(none.correlation, 0.0, 1e-12));
  CHECK(!std::isnan(none.rmse));
}

static void test_alignment_tolerance() {
  Series left = {{110, 1}, {210, 2}, {310, 3}};
  Series right = {{100, 10}, {200, 20}, {300, 30}};

  PairStats within = pair_stats(left, right, JoinKind::AsOfBackward, 20);
  CHECK(within.n == 3);
  CHECK(close_to(within.correlation, 1.0, 1e-9));
  CHECK(close_to(within.slope, 10.0, 1e-9));

  PairStats outside = pair_stats(left, right, JoinKind::AsOfBackward, 5);
  CHECK(outside.n == 0);
  CHECK(close_to(outside.correlation, 0.0, 1e-12));
  CHECK(!std::isnan(outside.slope));
}

static void test_empty_inputs() {
  PairStats a = pair_stats({}, {}, JoinKind::Inner, 0);
  CHECK(a.n == 0);
  CHECK(close_to(a.correlation, 0.0, 1e-12));

  Series left = {{10, 1}, {20, 2}};
  PairStats b = pair_stats(left, {}, JoinKind::Inner, 0);
  CHECK(b.n == 0);
  CHECK(close_to(b.correlation, 0.0, 1e-12));
  CHECK(!std::isnan(b.rmse));
}

static DecodedRecording* decode(const char* script,
                                DecodedRecording& rec) {
  AssembleResult a = assemble_script(script);
  for (const AssembleDiagnostic& d : a.errors) {
    std::fprintf(stderr, "assemble: %s\n",
                 describe_assemble_diagnostic(d).c_str());
  }
  CHECK(a.ok());
  rec.decode(a.bytes.data(), a.bytes.size());
  return &rec;
}

static const char* kMatrixScript =
    "device 1\n"
    "session 1\n"
    "ts 1000\n"
    "session-begin label=a\n"
    "schema id=1 name=m fields=x:u16,y:u16\n"
    "schema id=2 name=n fields=z:u16\n"
    "samples channel=1 delta=0 ts=1000\n"
    "  row 1 10\n"
    "end\n"
    "samples channel=1 delta=0 ts=1100\n"
    "  row 2 20\n"
    "end\n"
    "samples channel=1 delta=0 ts=1200\n"
    "  row 3 30\n"
    "end\n"
    "samples channel=1 delta=0 ts=1300\n"
    "  row 4 40\n"
    "end\n"
    "samples channel=2 delta=0 ts=1000\n"
    "  row 1\n"
    "end\n"
    "samples channel=2 delta=0 ts=1100\n"
    "  row 2\n"
    "end\n"
    "samples channel=2 delta=0 ts=1200\n"
    "  row 1\n"
    "end\n"
    "samples channel=2 delta=0 ts=1300\n"
    "  row 4\n"
    "end\n"
    "session-end reason=0\n";

static void test_enumerate_fields() {
  DecodedRecording rec;
  decode(kMatrixScript, rec);
  std::vector<ChannelFieldRef> refs = enumerate_fields(rec.channels());
  CHECK(refs.size() == 3);
  if (refs.size() == 3) {
    CHECK(same_ref(refs[0], 1, 0));
    CHECK(same_ref(refs[1], 1, 1));
    CHECK(same_ref(refs[2], 2, 0));
  }
}

static void test_correlation_matrix() {
  DecodedRecording rec;
  decode(kMatrixScript, rec);

  std::vector<PairResult> matrix = correlation_matrix(rec.channels(), 0);
  CHECK(matrix.size() == 3);
  if (matrix.size() != 3) return;

  CHECK(same_ref(matrix[0].a, 1, 0) && same_ref(matrix[0].b, 1, 1));
  CHECK(same_ref(matrix[1].a, 1, 0) && same_ref(matrix[1].b, 2, 0));
  CHECK(same_ref(matrix[2].a, 1, 1) && same_ref(matrix[2].b, 2, 0));

  CHECK(matrix[0].stats.n == 4);
  CHECK(close_to(matrix[0].stats.correlation, 1.0, 1e-9));
  CHECK(close_to(matrix[0].stats.slope, 10.0, 1e-9));

  CHECK(close_to(matrix[1].stats.correlation, 0.73029674, 1e-6));
  CHECK(close_to(matrix[2].stats.correlation, 0.73029674, 1e-6));

  std::vector<PairResult> top1 = top_correlated(matrix, 1);
  CHECK(top1.size() == 1);
  if (top1.size() == 1) {
    CHECK(same_ref(top1[0].a, 1, 0) && same_ref(top1[0].b, 1, 1));
  }

  std::vector<PairResult> top2 = top_correlated(matrix, 2);
  CHECK(top2.size() == 2);
  if (top2.size() == 2) {
    CHECK(same_ref(top2[0].a, 1, 0) && same_ref(top2[0].b, 1, 1));
    CHECK(same_ref(top2[1].a, 1, 0) && same_ref(top2[1].b, 2, 0));
  }

  std::vector<PairResult> top_all = top_correlated(matrix, 99);
  CHECK(top_all.size() == 3);

  std::vector<PairResult> strong = filter_pairs(matrix, 0.9);
  CHECK(strong.size() == 1);

  MatrixSummary summary = summarize_matrix(matrix);
  CHECK(summary.pair_count == 3);
  CHECK(summary.evaluated == 3);
  CHECK(summary.has_strongest);
  CHECK(summary.strongest_index == 0);
  CHECK(close_to(summary.max_abs_correlation, 1.0, 1e-9));
  CHECK(summary.mean_abs_correlation > 0.0);
  CHECK(summary.mean_abs_correlation < 1.0);

  std::string rendered = render_matrix(matrix);
  CHECK(rendered.find("3 pairs") != std::string::npos);
  CHECK(rendered.find("ch1[0]") != std::string::npos);
  CHECK(render_matrix({}) == "0 pairs\n");
  CHECK(!describe_pair(matrix[0]).empty());
  CHECK(describe_ref(matrix[0].a) == "ch1[0]");
}

static void test_matrix_empty_store() {
  DecodedRecording rec;
  std::vector<PairResult> matrix = correlation_matrix(rec.channels(), 0);
  CHECK(matrix.empty());
  CHECK(top_correlated(matrix, 3).empty());

  MatrixSummary summary = summarize_matrix(matrix);
  CHECK(summary.pair_count == 0);
  CHECK(summary.evaluated == 0);
  CHECK(!summary.has_strongest);
  CHECK(close_to(summary.mean_abs_correlation, 0.0, 1e-12));
}

static void test_lag_detection() {
  Series left = {{0, 1},  {10, 2}, {20, 4}, {30, 8},
                 {40, 7}, {50, 5}, {60, 3}};
  Series right = {{0, 9},  {10, 1}, {20, 2}, {30, 4},
                  {40, 8}, {50, 7}, {60, 5}};

  std::vector<LagScore> profile =
      lag_profile(left, right, JoinKind::Inner, 0, 3);
  CHECK(profile.size() == 7);
  if (profile.size() == 7) {
    CHECK(profile.front().lag == -3);
    CHECK(profile.back().lag == 3);
    CHECK(profile[3].lag == 0);
    CHECK(profile[3].overlap == 7);
  }

  LagScore best = best_lag(left, right, JoinKind::Inner, 0, 3);
  CHECK(best.lag == 1);
  CHECK(close_to(best.correlation, 1.0, 1e-9));
  CHECK(best.overlap == 6);

  CHECK(lag_profile(left, right, JoinKind::Inner, 0, -1).empty());
  LagScore degenerate = best_lag({}, {}, JoinKind::Inner, 0, 2);
  CHECK(degenerate.lag == 0);
  CHECK(close_to(degenerate.correlation, 0.0, 1e-12));
}

static void test_rank_ties_stable() {
  std::vector<PairResult> pairs(3);
  pairs[0].a = {1, 0};
  pairs[0].b = {1, 1};
  pairs[0].stats.correlation = 0.5;
  pairs[1].a = {1, 0};
  pairs[1].b = {2, 0};
  pairs[1].stats.correlation = -0.5;
  pairs[2].a = {1, 1};
  pairs[2].b = {2, 0};
  pairs[2].stats.correlation = 0.9;

  std::vector<size_t> order = rank_pairs(pairs);
  CHECK(order.size() == 3);
  if (order.size() == 3) {
    CHECK(order[0] == 2);
    CHECK(order[1] == 0);
    CHECK(order[2] == 1);
  }
}

int main() {
  test_perfect_positive();
  test_perfect_negative();
  test_uncorrelated();
  test_zero_variance_left();
  test_zero_variance_right();
  test_aligned_direct();
  test_alignment_tolerance();
  test_empty_inputs();
  test_enumerate_fields();
  test_correlation_matrix();
  test_matrix_empty_store();
  test_lag_detection();
  test_rank_ties_stable();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_xchannel: all checks passed\n");
  return 0;
}
