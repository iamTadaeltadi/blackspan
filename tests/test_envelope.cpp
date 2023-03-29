#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/envelope.h"
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

static Series ramp(size_t n, uint64_t step) {
  Series s;
  for (size_t i = 0; i < n; ++i) {
    s.emplace_back(static_cast<uint64_t>(i) * step, static_cast<double>(i));
  }
  return s;
}

static bool envelopes_equal(const Envelope& a, const Envelope& b) {
  if (a.points.size() != b.points.size()) return false;
  if (!nearly(a.min_value, b.min_value)) return false;
  if (!nearly(a.max_value, b.max_value)) return false;
  if (!nearly(a.mean_value, b.mean_value)) return false;
  for (size_t i = 0; i < a.points.size(); ++i) {
    const EnvelopePoint& p = a.points[i];
    const EnvelopePoint& q = b.points[i];
    if (p.ts != q.ts) return false;
    if (!nearly(p.lo, q.lo)) return false;
    if (!nearly(p.hi, q.hi)) return false;
    if (!nearly(p.mean, q.mean)) return false;
    if (!nearly(p.band, q.band)) return false;
  }
  return true;
}

static void test_mode_names_roundtrip() {
  const EnvelopeMode modes[] = {EnvelopeMode::Centered, EnvelopeMode::Trailing};
  for (EnvelopeMode m : modes) {
    EnvelopeMode parsed;
    CHECK(parse_envelope_mode(envelope_mode_name(m), parsed));
    CHECK(parsed == m);
  }
  EnvelopeMode dummy;
  CHECK(!parse_envelope_mode("boxcar", dummy));
  CHECK(!parse_envelope_mode("", dummy));
}

static void test_ramp_bounds() {
  Series s = ramp(10, 10);
  Envelope env = compute_envelope(s, 3);
  CHECK(env.points.size() == 10);

  for (size_t i = 0; i < env.points.size(); ++i) {
    const EnvelopePoint& p = env.points[i];
    const double value = s[i].second;
    CHECK(p.lo <= value);
    CHECK(value <= p.hi);
    CHECK(p.lo <= p.mean && p.mean <= p.hi);
    CHECK(p.band >= 0.0);
  }

  const EnvelopePoint& mid = env.points[5];
  CHECK(nearly(mid.lo, 4.0));
  CHECK(nearly(mid.hi, 6.0));
  CHECK(nearly(mid.mean, 5.0));
  CHECK(nearly(mid.band, std::sqrt(2.0 / 3.0)));

  const EnvelopePoint& first = env.points[0];
  CHECK(nearly(first.lo, 0.0));
  CHECK(nearly(first.hi, 2.0));
  CHECK(nearly(first.mean, 1.0));

  const EnvelopePoint& last = env.points[9];
  CHECK(nearly(last.lo, 7.0));
  CHECK(nearly(last.hi, 9.0));
  CHECK(nearly(last.mean, 8.0));

  CHECK(nearly(env.min_value, 0.0));
  CHECK(nearly(env.max_value, 9.0));
  CHECK(nearly(env.mean_value, 4.5));
}

static void test_constant() {
  Series s;
  for (size_t i = 0; i < 8; ++i) s.emplace_back(i * 5, 7.0);
  Envelope env = compute_envelope(s, 4);
  CHECK(env.points.size() == 8);
  for (const EnvelopePoint& p : env.points) {
    CHECK(nearly(p.lo, 7.0));
    CHECK(nearly(p.hi, 7.0));
    CHECK(nearly(p.mean, 7.0));
    CHECK(nearly(p.band, 0.0));
  }
  CHECK(nearly(env.min_value, 7.0));
  CHECK(nearly(env.max_value, 7.0));
  CHECK(nearly(env.mean_value, 7.0));
  CHECK(nearly(envelope_mean_width(env), 0.0));
  CHECK(nearly(envelope_band_area(env), 0.0));
}

static void test_window_clamp() {
  Series s = {{0, 3}, {10, 1}, {20, 4}, {30, 1}, {40, 5}};

  Envelope wide = compute_envelope(s, 100);
  CHECK(wide.points.size() == 5);
  for (const EnvelopePoint& p : wide.points) {
    CHECK(nearly(p.lo, 1.0));
    CHECK(nearly(p.hi, 5.0));
    CHECK(nearly(p.mean, 2.8));
  }

  Envelope zero = compute_envelope(s, 0);
  CHECK(zero.points.size() == 5);
  for (size_t i = 0; i < zero.points.size(); ++i) {
    const EnvelopePoint& p = zero.points[i];
    CHECK(nearly(p.lo, s[i].second));
    CHECK(nearly(p.hi, s[i].second));
    CHECK(nearly(p.mean, s[i].second));
    CHECK(nearly(p.band, 0.0));
  }

  Envelope one = compute_envelope(s, 1);
  CHECK(envelopes_equal(zero, one));
}

static void test_overall_stats() {
  Series s = {{0, 2}, {1, 8}, {2, 4}, {3, 6}};
  Envelope env = compute_envelope(s, 2);
  CHECK(nearly(env.min_value, 2.0));
  CHECK(nearly(env.max_value, 8.0));
  CHECK(nearly(env.mean_value, 5.0));
}

static void test_breaches_and_coverage() {
  Series s = ramp(12, 10);
  Envelope env = compute_envelope(s, 5);

  CHECK(envelope_breaches(s, env).empty());
  CHECK(nearly(envelope_coverage(s, env), 1.0));

  Series shifted = s;
  for (auto& p : shifted) p.second += 100.0;
  std::vector<EnvelopeBreach> br = envelope_breaches(shifted, env);
  CHECK(br.size() == shifted.size());
  if (!br.empty()) {
    CHECK(br.front().index == 0);
    CHECK(nearly(br.front().value, shifted.front().second));
    CHECK(br.front().value > br.front().hi);
    CHECK(br.front().ts == shifted.front().first);
  }
  CHECK(nearly(envelope_coverage(shifted, env), 0.0));
}

static void test_breach_runs() {
  Series s = ramp(10, 10);
  Envelope env = compute_envelope(s, 3);

  Series probe = s;
  probe[3].second += 100.0;
  probe[4].second += 100.0;
  probe[7].second += 100.0;

  std::vector<BreachRun> runs = envelope_breach_runs(probe, env);
  CHECK(runs.size() == 2);
  if (runs.size() == 2) {
    CHECK(runs[0].start_index == 3);
    CHECK(runs[0].end_index == 4);
    CHECK(runs[0].count == 2);
    CHECK(runs[0].start_ts == probe[3].first);
    CHECK(runs[0].end_ts == probe[4].first);
    CHECK(runs[0].max_excess > 0.0);

    CHECK(runs[1].start_index == 7);
    CHECK(runs[1].end_index == 7);
    CHECK(runs[1].count == 1);
    CHECK(runs[1].max_excess > 0.0);
  }

  CHECK(envelope_breach_runs(s, env).empty());
}

static void test_trailing_mode() {
  Series s = ramp(8, 10);
  Envelope env = compute_envelope(s, 3, EnvelopeMode::Trailing);
  CHECK(env.points.size() == 8);

  for (size_t i = 0; i < env.points.size(); ++i) {
    const EnvelopePoint& p = env.points[i];
    CHECK(p.lo <= s[i].second);
    CHECK(s[i].second <= p.hi);
  }

  const EnvelopePoint& p0 = env.points[0];
  CHECK(nearly(p0.lo, 0.0));
  CHECK(nearly(p0.hi, 0.0));
  CHECK(nearly(p0.mean, 0.0));

  const EnvelopePoint& p1 = env.points[1];
  CHECK(nearly(p1.lo, 0.0));
  CHECK(nearly(p1.hi, 1.0));
  CHECK(nearly(p1.mean, 0.5));

  const EnvelopePoint& p5 = env.points[5];
  CHECK(nearly(p5.lo, 3.0));
  CHECK(nearly(p5.hi, 5.0));
  CHECK(nearly(p5.mean, 4.0));
}

static void test_sigma_envelope() {
  Series s = ramp(12, 10);
  Envelope base = compute_envelope(s, 5);
  Envelope sig = compute_sigma_envelope(s, 5, 2.0, EnvelopeMode::Centered);
  CHECK(sig.points.size() == base.points.size());

  for (size_t i = 0; i < sig.points.size(); ++i) {
    const EnvelopePoint& p = sig.points[i];
    CHECK(nearly(p.band, base.points[i].band));
    CHECK(nearly(p.mean, base.points[i].mean));
    CHECK(nearly(p.lo, p.mean - 2.0 * p.band));
    CHECK(nearly(p.hi, p.mean + 2.0 * p.band));
    CHECK(p.lo <= p.mean && p.mean <= p.hi);
  }

  Series c;
  for (size_t i = 0; i < 5; ++i) c.emplace_back(i, 42.0);
  Envelope csig = compute_sigma_envelope(c, 3, 3.0, EnvelopeMode::Centered);
  for (const EnvelopePoint& p : csig.points) {
    CHECK(nearly(p.lo, 42.0));
    CHECK(nearly(p.hi, 42.0));
    CHECK(nearly(p.band, 0.0));
  }

  Envelope neg = compute_sigma_envelope(s, 5, -2.0, EnvelopeMode::Centered);
  for (const EnvelopePoint& p : neg.points) CHECK(p.lo <= p.hi);
}

static void test_envelope_at() {
  Series s = ramp(5, 10);
  Envelope env = compute_envelope(s, 1);

  EnvelopePoint before = envelope_at(env, 0);
  CHECK(nearly(before.mean, 0.0));

  EnvelopePoint mid = envelope_at(env, 5);
  CHECK(mid.ts == 5);
  CHECK(nearly(mid.mean, 0.5));
  CHECK(nearly(mid.lo, 0.5));
  CHECK(nearly(mid.hi, 0.5));

  EnvelopePoint exact = envelope_at(env, 20);
  CHECK(nearly(exact.mean, 2.0));

  EnvelopePoint after = envelope_at(env, 999);
  CHECK(nearly(after.mean, 4.0));

  Envelope empty;
  EnvelopePoint none = envelope_at(empty, 100);
  CHECK(nearly(none.mean, 0.0));
  CHECK(none.ts == 0);
}

static void test_band_area() {
  Series s = {{0, 0}, {10, 10}, {20, 0}};
  Envelope env = compute_envelope(s, 3);
  double area = envelope_band_area(env);
  double expected = 0.0;
  for (size_t i = 0; i + 1 < env.points.size(); ++i) {
    const double wa = env.points[i].hi - env.points[i].lo;
    const double wb = env.points[i + 1].hi - env.points[i + 1].lo;
    const double dt = static_cast<double>(env.points[i + 1].ts) -
                      static_cast<double>(env.points[i].ts);
    expected += 0.5 * (wa + wb) * dt;
  }
  CHECK(nearly(area, expected));
  CHECK(area > 0.0);
}

static void test_empty_and_single() {
  Envelope empty = compute_envelope({}, 4);
  CHECK(empty.points.empty());
  CHECK(nearly(empty.min_value, 0.0));
  CHECK(nearly(empty.max_value, 0.0));
  CHECK(nearly(empty.mean_value, 0.0));
  CHECK(envelope_breaches({}, empty).empty());
  CHECK(envelope_breach_runs({}, empty).empty());
  CHECK(nearly(envelope_mean_width(empty), 0.0));
  CHECK(nearly(envelope_band_area(empty), 0.0));
  CHECK(nearly(envelope_coverage({}, empty), 1.0));
  CHECK(describe_envelope(empty) == "envelope: empty\n");

  Series one = {{50, 7.0}};
  Envelope env = compute_envelope(one, 4);
  CHECK(env.points.size() == 1);
  CHECK(nearly(env.points[0].lo, 7.0));
  CHECK(nearly(env.points[0].hi, 7.0));
  CHECK(nearly(env.points[0].mean, 7.0));
  CHECK(nearly(env.points[0].band, 0.0));
  CHECK(nearly(env.min_value, 7.0));
  CHECK(nearly(env.max_value, 7.0));
  CHECK(nearly(env.mean_value, 7.0));
  CHECK(nearly(envelope_band_area(env), 0.0));
  CHECK(envelope_breaches(one, env).empty());
}

static void test_describe() {
  Series s = ramp(6, 10);
  Envelope env = compute_envelope(s, 3);
  std::string a = describe_envelope(env);
  std::string b = describe_envelope(env);
  CHECK(a == b);
  CHECK(a.find("points") != std::string::npos);
  CHECK(a.find("min=") != std::string::npos);
  CHECK(a.back() == '\n');
}

static void test_adapter() {
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

  Series pairs = {{1000, 10}, {1000, 20}, {1100, 30}, {1100, 40}};

  Envelope expected = compute_envelope(pairs, 3);
  Envelope by_index = channel_envelope(rec.channels(), 1, size_t{0}, 3);
  CHECK(envelopes_equal(by_index, expected));

  Envelope sig_expected =
      compute_sigma_envelope(pairs, 3, 2.0, EnvelopeMode::Centered);
  Envelope sig_index = channel_sigma_envelope(rec.channels(), 1, size_t{0}, 3, 2.0);
  CHECK(envelopes_equal(sig_index, sig_expected));

  CHECK(nearly(by_index.min_value, 10.0));
  CHECK(nearly(by_index.max_value, 40.0));

  Envelope bad_ch = channel_envelope(rec.channels(), 99, size_t{0}, 3);
  CHECK(bad_ch.points.empty());
  Envelope bad_field = channel_envelope(rec.channels(), 1, size_t{5}, 3);
  CHECK(bad_field.points.empty());
}

int main() {
  test_mode_names_roundtrip();
  test_ramp_bounds();
  test_constant();
  test_window_clamp();
  test_overall_stats();
  test_breaches_and_coverage();
  test_breach_runs();
  test_trailing_mode();
  test_sigma_envelope();
  test_envelope_at();
  test_band_area();
  test_empty_and_single();
  test_describe();
  test_adapter();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_envelope: all checks passed\n");
  return 0;
}
