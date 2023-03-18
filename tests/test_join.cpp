#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/join.h"
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

using Series = std::vector<std::pair<uint64_t, double>>;

// One joined row matches an expected tuple.
static bool row_is(const JoinedRow& r, uint64_t ts, double left, double right,
                   uint64_t right_ts) {
  return r.ts == ts && nearly(r.left, left) && nearly(r.right, right) &&
         r.right_ts == right_ts;
}

// Two join results match: same rows in order, same tallies.
static bool result_equal(const JoinResult& a, const JoinResult& b) {
  if (a.left_unmatched != b.left_unmatched) return false;
  if (a.right_unmatched != b.right_unmatched) return false;
  if (a.rows.size() != b.rows.size()) return false;
  for (size_t i = 0; i < a.rows.size(); ++i) {
    const JoinedRow& x = a.rows[i];
    const JoinedRow& y = b.rows[i];
    if (x.ts != y.ts || x.right_ts != y.right_ts) return false;
    if (!nearly(x.left, y.left) || !nearly(x.right, y.right)) return false;
  }
  return true;
}

static void test_names_roundtrip() {
  const JoinKind kinds[] = {JoinKind::Inner, JoinKind::AsOfBackward,
                            JoinKind::AsOfForward};
  for (JoinKind k : kinds) {
    JoinKind parsed;
    CHECK(parse_join_kind(join_kind_name(k), parsed));
    CHECK(parsed == k);
  }
  JoinKind dummy;
  CHECK(!parse_join_kind("outer", dummy));
  CHECK(!parse_join_kind("asof", dummy));
  CHECK(!parse_join_kind("", dummy));
}

static void test_inner_overlap() {
  // Left ts {0,10,20,30}, right ts {10,20,25,30}. Shared: 10,20,30.
  Series left = {{0, 0}, {10, 1}, {20, 2}, {30, 3}};
  Series right = {{10, 100}, {20, 200}, {25, 250}, {30, 300}};
  JoinResult res = join_series(left, right, JoinKind::Inner, 0);

  CHECK(res.rows.size() == 3);
  if (res.rows.size() == 3) {
    CHECK(row_is(res.rows[0], 10, 1, 100, 10));
    CHECK(row_is(res.rows[1], 20, 2, 200, 20));
    CHECK(row_is(res.rows[2], 30, 3, 300, 30));
  }
  CHECK(res.left_unmatched == 1);   // left ts 0
  CHECK(res.right_unmatched == 1);  // right ts 25

  // Inner ignores tolerance: a nonzero bound changes nothing.
  JoinResult tol = join_series(left, right, JoinKind::Inner, 1);
  CHECK(result_equal(res, tol));
}

static void test_inner_disjoint() {
  // No shared timestamps at all: every sample is unmatched, no rows.
  Series left = {{0, 1}, {10, 2}};
  Series right = {{5, 3}, {15, 4}};
  JoinResult res = join_series(left, right, JoinKind::Inner, 0);
  CHECK(res.rows.empty());
  CHECK(res.left_unmatched == 2);
  CHECK(res.right_unmatched == 2);
}

static void test_inner_duplicates() {
  // Duplicate timestamps on both sides: each side reduces to its last value at a
  // shared timestamp before matching.
  Series left = {{10, 1}, {10, 2}, {20, 5}};
  Series right = {{10, 7}, {10, 9}, {20, 8}};
  JoinResult res = join_series(left, right, JoinKind::Inner, 0);
  CHECK(res.rows.size() == 2);
  if (res.rows.size() == 2) {
    CHECK(row_is(res.rows[0], 10, 2, 9, 10));  // last of each run at ts 10
    CHECK(row_is(res.rows[1], 20, 5, 8, 20));
  }
  CHECK(res.left_unmatched == 0);
  CHECK(res.right_unmatched == 0);
}

static void test_asof_backward() {
  Series right = {{10, 100}, {20, 200}, {30, 300}};
  // 5: nothing at or before -> unmatched. 10: exact. 15: carries ts 10.
  // 20: exact. 35: carries ts 30.
  Series left = {{5, 1}, {10, 2}, {15, 3}, {20, 4}, {35, 5}};
  JoinResult res = join_series(left, right, JoinKind::AsOfBackward, 0);

  CHECK(res.rows.size() == 4);
  if (res.rows.size() == 4) {
    CHECK(row_is(res.rows[0], 10, 2, 100, 10));  // exact hit pairs with itself
    CHECK(row_is(res.rows[1], 15, 3, 100, 10));  // between 10 and 20 -> earlier
    CHECK(row_is(res.rows[2], 20, 4, 200, 20));
    CHECK(row_is(res.rows[3], 35, 5, 300, 30));  // past the end -> last sample
  }
  CHECK(res.left_unmatched == 1);   // ts 5 had no right at or before
  CHECK(res.right_unmatched == 0);  // all three right samples were used

  // A right sample no left reaches back to is right_unmatched.
  Series one = {{25, 9}};
  JoinResult r2 = join_series(one, right, JoinKind::AsOfBackward, 0);
  CHECK(r2.rows.size() == 1);
  if (r2.rows.size() == 1) CHECK(row_is(r2.rows[0], 25, 9, 200, 20));
  CHECK(r2.right_unmatched == 2);  // ts 10 and ts 30 never selected
}

static void test_asof_backward_tolerance() {
  Series right = {{10, 100}, {20, 200}, {30, 300}};
  // 28 reaches back to 20 (gap 8) -> excluded by tol 5.
  JoinResult far = join_series(Series{{28, 1}}, right, JoinKind::AsOfBackward, 5);
  CHECK(far.rows.empty());
  CHECK(far.left_unmatched == 1);
  CHECK(far.right_unmatched == 3);

  // 33 reaches back to 30 (gap 3 <= 5) -> matched.
  JoinResult near = join_series(Series{{33, 1}}, right, JoinKind::AsOfBackward, 5);
  CHECK(near.rows.size() == 1);
  if (near.rows.size() == 1) CHECK(row_is(near.rows[0], 33, 1, 300, 30));
  CHECK(near.left_unmatched == 0);
}

static void test_asof_forward() {
  Series right = {{10, 100}, {20, 200}, {30, 300}};
  // 5: earliest >= 5 is 10. 10: exact. 15: carries ts 20. 20: exact.
  // 35: nothing at or after -> unmatched.
  Series left = {{5, 1}, {10, 2}, {15, 3}, {20, 4}, {35, 5}};
  JoinResult res = join_series(left, right, JoinKind::AsOfForward, 0);

  CHECK(res.rows.size() == 4);
  if (res.rows.size() == 4) {
    CHECK(row_is(res.rows[0], 5, 1, 100, 10));   // before the start -> earliest
    CHECK(row_is(res.rows[1], 10, 2, 100, 10));  // exact hit pairs with itself
    CHECK(row_is(res.rows[2], 15, 3, 200, 20));  // between 10 and 20 -> later
    CHECK(row_is(res.rows[3], 20, 4, 200, 20));
  }
  CHECK(res.left_unmatched == 1);   // ts 35 had no right at or after
  CHECK(res.right_unmatched == 1);  // ts 30 never selected
}

static void test_asof_forward_tolerance() {
  Series right = {{10, 100}, {20, 200}, {30, 300}};
  // 12 reaches forward to 20 (gap 8) -> excluded by tol 5.
  JoinResult far = join_series(Series{{12, 1}}, right, JoinKind::AsOfForward, 5);
  CHECK(far.rows.empty());
  CHECK(far.left_unmatched == 1);

  // 16 reaches forward to 20 (gap 4 <= 5) -> matched.
  JoinResult near = join_series(Series{{16, 1}}, right, JoinKind::AsOfForward, 5);
  CHECK(near.rows.size() == 1);
  if (near.rows.size() == 1) CHECK(row_is(near.rows[0], 16, 1, 200, 20));
}

static void test_tolerance_boundary_inclusive() {
  // A match at exactly tolerance_ns is accepted; one nanosecond past it is not.
  Series rb = {{10, 100}};
  CHECK(join_series(Series{{15, 1}}, rb, JoinKind::AsOfBackward, 5).rows.size() == 1);
  CHECK(join_series(Series{{16, 1}}, rb, JoinKind::AsOfBackward, 5).rows.empty());

  Series rf = {{20, 200}};
  CHECK(join_series(Series{{15, 1}}, rf, JoinKind::AsOfForward, 5).rows.size() == 1);
  CHECK(join_series(Series{{14, 1}}, rf, JoinKind::AsOfForward, 5).rows.empty());
}

static void test_asof_duplicate_right_ts() {
  // Two right samples share ts 20 (values 200 then 201). Backward takes the last
  // recorded; forward takes the first.
  Series right = {{10, 100}, {20, 200}, {20, 201}, {30, 300}};
  JoinResult back = join_series(Series{{25, 1}}, right, JoinKind::AsOfBackward, 0);
  CHECK(back.rows.size() == 1);
  if (back.rows.size() == 1) CHECK(row_is(back.rows[0], 25, 1, 201, 20));

  JoinResult fwd = join_series(Series{{15, 1}}, right, JoinKind::AsOfForward, 0);
  CHECK(fwd.rows.size() == 1);
  if (fwd.rows.size() == 1) CHECK(row_is(fwd.rows[0], 15, 1, 200, 20));
}

static void test_degenerate() {
  Series s = {{10, 1}, {20, 2}};

  // Empty right: every left sample is unmatched under an as-of join; inner emits
  // nothing and counts the left's distinct timestamps.
  JoinResult be = join_series(s, {}, JoinKind::AsOfBackward, 0);
  CHECK(be.rows.empty() && be.left_unmatched == 2 && be.right_unmatched == 0);
  JoinResult ie = join_series(s, {}, JoinKind::Inner, 0);
  CHECK(ie.rows.empty() && ie.left_unmatched == 2 && ie.right_unmatched == 0);

  // Empty left: no rows; every right sample is unmatched.
  JoinResult el = join_series({}, s, JoinKind::AsOfForward, 0);
  CHECK(el.rows.empty() && el.left_unmatched == 0 && el.right_unmatched == 2);

  // Both empty.
  JoinResult ee = join_series({}, {}, JoinKind::Inner, 0);
  CHECK(ee.rows.empty() && ee.left_unmatched == 0 && ee.right_unmatched == 0);

  // Single element on each side, matching exactly.
  JoinResult one =
      join_series(Series{{5, 7}}, Series{{5, 9}}, JoinKind::Inner, 0);
  CHECK(one.rows.size() == 1);
  if (one.rows.size() == 1) CHECK(row_is(one.rows[0], 5, 7, 9, 5));
}

static void test_unsorted_matches_sorted() {
  Series left_sorted = {{0, 0}, {10, 1}, {20, 2}, {30, 3}};
  Series right_sorted = {{5, 50}, {10, 100}, {25, 250}, {30, 300}};
  Series left_shuf = {{30, 3}, {0, 0}, {20, 2}, {10, 1}};
  Series right_shuf = {{25, 250}, {5, 50}, {30, 300}, {10, 100}};

  for (JoinKind k : {JoinKind::Inner, JoinKind::AsOfBackward,
                     JoinKind::AsOfForward}) {
    JoinResult a = join_series(left_sorted, right_sorted, k, 0);
    JoinResult b = join_series(left_shuf, right_shuf, k, 0);
    CHECK(result_equal(a, b));
  }
}

// Builds a two-channel recording and confirms the ChannelStore adapter matches
// the core run over the same reconstructed (block_ts, value) pairs.
static void test_adapter() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "schema id=2 name=n fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"  // base ts 1000
      "  row 10\n"
      "  row 20\n"
      "end\n"
      "samples channel=1 delta=0 ts=1100\n"  // base ts 1100
      "  row 30\n"
      "end\n"
      "samples channel=2 delta=0 ts=1000\n"  // base ts 1000
      "  row 5\n"
      "end\n"
      "samples channel=2 delta=0 ts=1100\n"  // base ts 1100
      "  row 7\n"
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
  Series left = {{1000, 10}, {1000, 20}, {1100, 30}};
  Series right = {{1000, 5}, {1100, 7}};

  for (JoinKind k : {JoinKind::Inner, JoinKind::AsOfBackward,
                     JoinKind::AsOfForward}) {
    JoinResult expected = join_series(left, right, k, 0);
    JoinResult got = join_channels(rec.channels(), 1, 0, 2, 0, k, 0);
    CHECK(result_equal(expected, got));
  }

  // Concrete inner spot check: ts 1000 reduces to left's last value (20).
  JoinResult inner = join_channels(rec.channels(), 1, 0, 2, 0, JoinKind::Inner, 0);
  CHECK(inner.rows.size() == 2);
  if (inner.rows.size() == 2) {
    CHECK(row_is(inner.rows[0], 1000, 20, 5, 1000));
    CHECK(row_is(inner.rows[1], 1100, 30, 7, 1100));
  }

  // Unknown left channel: left side empty. Inner emits nothing and counts the
  // right's distinct timestamps as unmatched.
  JoinResult bad_left =
      join_channels(rec.channels(), 99, 0, 2, 0, JoinKind::Inner, 0);
  CHECK(bad_left.rows.empty());
  CHECK(bad_left.left_unmatched == 0);
  CHECK(bad_left.right_unmatched == 2);  // ts 1000 and 1100

  // Unknown left channel under an as-of join: right samples are all unmatched.
  JoinResult bad_left_asof =
      join_channels(rec.channels(), 99, 0, 2, 0, JoinKind::AsOfBackward, 0);
  CHECK(bad_left_asof.rows.empty());
  CHECK(bad_left_asof.right_unmatched == 2);

  // Out-of-range right field: right side empty.
  JoinResult bad_field =
      join_channels(rec.channels(), 1, 0, 2, 5, JoinKind::AsOfForward, 0);
  CHECK(bad_field.rows.empty());
  CHECK(bad_field.left_unmatched == 3);  // three left samples, none matched
}

int main() {
  test_names_roundtrip();
  test_inner_overlap();
  test_inner_disjoint();
  test_inner_duplicates();
  test_asof_backward();
  test_asof_backward_tolerance();
  test_asof_forward();
  test_asof_forward_tolerance();
  test_tolerance_boundary_inclusive();
  test_asof_duplicate_right_ts();
  test_degenerate();
  test_unsorted_matches_sorted();
  test_adapter();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_join: all checks passed\n");
  return 0;
}
