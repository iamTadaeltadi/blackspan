#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/byteio.h"
#include "blackspan/channel.h"
#include "blackspan/fold.h"
#include "blackspan/frame.h"
#include "blackspan/schema.h"

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

static bool close_to(double a, double b) { return std::fabs(a - b) < 1e-9; }
static bool near_eps(double a, double b, double eps) {
  return std::fabs(a - b) < eps;
}

static const double kTwoPi = 2.0 * std::acos(-1.0);

static std::vector<double> repeat_pattern(const std::vector<double>& pat,
                                          size_t cycles) {
  std::vector<double> out;
  out.reserve(pat.size() * cycles);
  for (size_t c = 0; c < cycles; ++c) {
    for (double v : pat) out.push_back(v);
  }
  return out;
}

static std::vector<double> sine_series(size_t n, size_t period) {
  std::vector<double> out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    out.push_back(std::sin(kTwoPi * static_cast<double>(i) /
                           static_cast<double>(period)));
  }
  return out;
}

static std::vector<double> ramp_series(size_t n) {
  std::vector<double> out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) out.push_back(static_cast<double>(i));
  return out;
}

namespace {

void put_frame(std::vector<uint8_t>& wire, RecordType type, uint32_t device_id,
               uint32_t seq, uint64_t ts, const std::vector<uint8_t>& payload) {
  FrameHeader h;
  h.type = type;
  h.device_id = device_id;
  h.session_seq = seq;
  h.monotonic_ts = ts;
  encode_frame(h, payload.data(), payload.size(), wire);
}

void build_periodic_channel(SchemaRegistry& schemas, ChannelStore& channels,
                            const std::vector<double>& pattern, size_t cycles) {
  std::vector<uint8_t> wire;
  {
    ByteWriter p;
    p.u16(1);
    p.u8(1);
    p.u8(1);
    const std::string cname = "sensor";
    p.u8(static_cast<uint8_t>(cname.size()));
    p.bytes(reinterpret_cast<const uint8_t*>(cname.data()), cname.size());
    p.u8(3);
    const std::string fname = "level";
    p.u8(static_cast<uint8_t>(fname.size()));
    p.bytes(reinterpret_cast<const uint8_t*>(fname.data()), fname.size());
    put_frame(wire, RecordType::SchemaDef, 0x100, 0, 1000, p.buf());
  }
  {
    ByteWriter p;
    const size_t total = pattern.size() * cycles;
    p.u16(1);
    p.u16(1);
    p.u32(0);
    p.u16(static_cast<uint16_t>(total));
    p.u16(0);
    for (size_t c = 0; c < cycles; ++c) {
      for (double v : pattern) {
        p.u16(static_cast<uint16_t>(v));
      }
    }
    put_frame(wire, RecordType::SampleBlock, 0x100, 1, 1001, p.buf());
  }
  FrameStream fs(wire.data(), wire.size());
  Frame f;
  while (fs.next(f)) {
    if (f.header.type == RecordType::SchemaDef) schemas.ingest_frame(f);
    if (f.header.type == RecordType::SampleBlock)
      channels.ingest_frame(f, schemas);
  }
}

}

static void test_estimate_period_clean() {
  const std::vector<double> sine = sine_series(70, 7);
  PeriodEstimate se = estimate_period(sine, 2, 35);
  CHECK(se.found);
  CHECK(se.period == 7);
  CHECK(se.strength > 0.99);

  std::vector<double> saw;
  for (size_t i = 0; i < 50; ++i) saw.push_back(static_cast<double>(i % 5));
  PeriodEstimate sa = estimate_period(saw, 2, 25);
  CHECK(sa.found);
  CHECK(sa.period == 5);
  CHECK(sa.strength > 0.99);

  const std::vector<double> pat = repeat_pattern({10, 40, 20, 55, 30, 5}, 12);
  PeriodEstimate pe = estimate_period(pat, 2, 30);
  CHECK(pe.found);
  CHECK(pe.period == 6);
}

static void test_estimate_period_nonperiodic() {
  const std::vector<double> ramp = ramp_series(40);
  PeriodEstimate re = estimate_period(ramp, 2, 20);
  CHECK(!re.found);
  CHECK(re.strength < 0.1);

  const std::vector<double> flat(40, 5.0);
  PeriodEstimate fe = estimate_period(flat, 2, 20);
  CHECK(!fe.found);
  CHECK(close_to(fe.strength, 0.0));

  PeriodEstimate empty = estimate_period({}, 1, 5);
  CHECK(!empty.found && empty.period == 0 && close_to(empty.strength, 0.0));

  PeriodEstimate single = estimate_period({9}, 1, 5);
  CHECK(!single.found && single.period == 0);

  PeriodEstimate tiny = estimate_period({1, 2, 3}, 1, 2);
  CHECK(!tiny.found && tiny.period == 0);

  const std::vector<double> ok = sine_series(40, 5);
  PeriodEstimate bad_range = estimate_period(ok, 9, 3);
  CHECK(!bad_range.found && bad_range.period == 0);
}

static void test_scan_and_harmonics() {
  const std::vector<double> sine = sine_series(70, 7);
  PeriodScan scan = scan_periods(sine, 2, 35);
  CHECK(scan.periods.size() == 34);
  CHECK(scan.autocorr.size() == scan.periods.size());
  CHECK(scan.score.size() == scan.periods.size());
  CHECK(scan.periods.front() == 2 && scan.periods.back() == 35);

  size_t best = 0;
  double best_ac = -1e18;
  for (size_t i = 0; i < scan.periods.size(); ++i) {
    if (scan.autocorr[i] > best_ac) {
      best_ac = scan.autocorr[i];
      best = scan.periods[i];
    }
    if (scan.periods[i] == 7) CHECK(scan.score[i] > 0.99);
  }
  CHECK(best == 7);

  CHECK(scan_periods({}, 1, 5).periods.empty());
  CHECK(scan_periods({1, 2, 3}, 1, 2).periods.empty());

  const std::vector<double> v = repeat_pattern({1, 2, 3}, 8);
  CHECK(fundamental_period(v, 6, 0.02) == 3);
  CHECK(fundamental_period(v, 12, 0.02) == 3);
  CHECK(fundamental_period(v, 3, 0.02) == 3);
  CHECK(fundamental_period(v, 0, 0.02) == 0);
  CHECK(fundamental_period(sine, 7, 0.02) == 7);

  PeriodEstimate he = estimate_period(v, 2, 12);
  CHECK(he.found && he.period == 3);
}

static void test_cycle_consistency() {
  const std::vector<double> consistent = repeat_pattern({2, 5, 9, 4}, 6);
  CHECK(near_eps(cycle_consistency(consistent, 4), 1.0, 1e-9));

  std::vector<double> flip;
  for (size_t c = 0; c < 4; ++c) {
    if (c % 2 == 0) {
      flip.push_back(1);
      flip.push_back(2);
      flip.push_back(3);
    } else {
      flip.push_back(3);
      flip.push_back(2);
      flip.push_back(1);
    }
  }
  CHECK(near_eps(cycle_consistency(flip, 3), -1.0, 1e-9));

  CHECK(close_to(cycle_consistency(consistent, 0), 0.0));
  CHECK(close_to(cycle_consistency({1, 2, 3}, 3), 0.0));
  CHECK(close_to(cycle_consistency({}, 4), 0.0));
}

static void test_fold_series() {
  const std::vector<double> v = repeat_pattern({1, 2, 3}, 4);
  FoldedProfile p = fold_series(v, 3);
  CHECK(p.period == 3);
  CHECK(p.cycles == 4);
  CHECK(p.mean.size() == 3 && p.band.size() == 3);
  CHECK(close_to(p.mean[0], 1.0) && close_to(p.mean[1], 2.0) &&
        close_to(p.mean[2], 3.0));
  CHECK(close_to(p.band[0], 0.0) && close_to(p.band[1], 0.0) &&
        close_to(p.band[2], 0.0));

  std::vector<double> noisy = repeat_pattern({1, 2, 3}, 4);
  noisy[0] = 3.0;
  noisy[3] = -1.0;
  FoldedProfile pn = fold_series(noisy, 3);
  CHECK(close_to(pn.mean[0], 1.0));
  CHECK(pn.band[0] > 0.0);
  CHECK(close_to(pn.band[1], 0.0));

  FoldedProfile z = fold_series(v, 0);
  CHECK(z.period == 0 && z.cycles == 0 && z.mean.empty() && z.band.empty());

  FoldedProfile sparse = fold_series({10, 20}, 5);
  CHECK(sparse.period == 5 && sparse.cycles == 0);
  CHECK(sparse.mean.size() == 5);
  CHECK(close_to(sparse.mean[0], 10.0) && close_to(sparse.mean[1], 20.0));
  CHECK(close_to(sparse.mean[2], 0.0) && close_to(sparse.mean[3], 0.0) &&
        close_to(sparse.mean[4], 0.0));

  FoldedProfile e = fold_series({}, 3);
  CHECK(e.period == 3 && e.cycles == 0 && e.mean.size() == 3);
  CHECK(close_to(e.mean[0], 0.0) && close_to(e.band[0], 0.0));
}

static void test_periodicity_score() {
  const std::vector<double> perfect = repeat_pattern({1, 2, 3}, 6);
  CHECK(near_eps(periodicity_score(perfect, 3), 1.0, 1e-9));

  const std::vector<double> ramp = ramp_series(12);
  CHECK(periodicity_score(ramp, 3) < 0.1);

  CHECK(close_to(periodicity_score(perfect, 0), 0.0));
  CHECK(close_to(periodicity_score({}, 3), 0.0));
  CHECK(close_to(periodicity_score({7, 7, 7, 7, 7, 7}, 3), 0.0));

  const std::vector<double> sine = sine_series(60, 6);
  CHECK(periodicity_score(sine, 6) > 0.99);
}

static void test_fold_residual() {
  const std::vector<double> perfect = repeat_pattern({1, 2, 3}, 4);
  FoldedProfile pp = fold_series(perfect, 3);
  ResidualStats rp = fold_residual(perfect, pp);
  CHECK(near_eps(rp.rms, 0.0, 1e-9));
  CHECK(near_eps(rp.max_abs, 0.0, 1e-9));
  CHECK(rp.worst_index == 0);

  std::vector<double> glitch = repeat_pattern({1, 2, 3}, 4);
  glitch[7] += 100.0;
  FoldedProfile pg = fold_series(glitch, 3);
  ResidualStats rg = fold_residual(glitch, pg);
  CHECK(rg.rms > 0.0);
  CHECK(rg.worst_index == 7);
  CHECK(near_eps(rg.max_abs, 75.0, 1e-9));

  ResidualStats empty = fold_residual({}, pp);
  CHECK(close_to(empty.rms, 0.0) && empty.worst_index == 0);

  FoldedProfile zero;
  ResidualStats none = fold_residual(perfect, zero);
  CHECK(close_to(none.rms, 0.0) && close_to(none.max_abs, 0.0));
}

static void test_profile_helpers() {
  const std::vector<double> v = repeat_pattern({1, 2, 3}, 4);
  FoldedProfile p = fold_series(v, 3);

  CHECK(close_to(profile_amplitude(p), 2.0));
  CHECK(profile_peak_phase(p) == 2);
  CHECK(profile_trough_phase(p) == 0);

  std::vector<double> model = reconstruct(p, 6);
  CHECK(model.size() == 6);
  CHECK(close_to(model[0], 1.0) && close_to(model[1], 2.0) &&
        close_to(model[2], 3.0) && close_to(model[3], 1.0));

  FoldedProfile z;
  CHECK(close_to(profile_amplitude(z), 0.0));
  CHECK(profile_peak_phase(z) == 0 && profile_trough_phase(z) == 0);
  std::vector<double> zmodel = reconstruct(z, 4);
  CHECK(zmodel.size() == 4 && close_to(zmodel[0], 0.0));

  std::string text = render_profile(p);
  CHECK(text.find("period 3") != std::string::npos);
  CHECK(text.find("phase 0") != std::string::npos);
}

static void test_adapter() {
  SchemaRegistry schemas;
  ChannelStore channels;
  build_periodic_channel(schemas, channels, {100, 400, 200, 550, 300, 50}, 10);

  std::vector<FoldSummary> folds = analyze_folds(channels);
  CHECK(folds.size() == 1);
  if (folds.size() == 1) {
    const FoldSummary& f = folds.front();
    CHECK(f.channel_id == 1);
    CHECK(f.field_index == 0);
    CHECK(f.samples == 60);
    CHECK(f.period == 6);
    CHECK(f.strength > 0.99);
    CHECK(f.residual_rms < 1e-6);

    std::string desc = describe_fold(f);
    CHECK(desc.find("channel 0001") != std::string::npos);
    CHECK(desc.find("period 6") != std::string::npos);
  }

  std::string rendered = render_folds(folds);
  CHECK(rendered.find("1 fold(s)") != std::string::npos);

  ChannelStore empty;
  CHECK(analyze_folds(empty).empty());
  CHECK(render_folds({}).find("0 fold(s)") != std::string::npos);
}

int main() {
  test_estimate_period_clean();
  test_estimate_period_nonperiodic();
  test_scan_and_harmonics();
  test_cycle_consistency();
  test_fold_series();
  test_periodicity_score();
  test_fold_residual();
  test_profile_helpers();
  test_adapter();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_fold: all checks passed\n");
  return 0;
}
