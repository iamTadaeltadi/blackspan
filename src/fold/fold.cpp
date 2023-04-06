#include "blackspan/fold.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "blackspan/stats_advanced.h"

namespace blackspan {

namespace {

constexpr double kFoundStrength = 0.5;
constexpr double kHarmonicTolerance = 0.02;
constexpr size_t kMinSamples = 4;

double clamp_unit(double v) {
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

bool close_enough(double a, double b) { return std::fabs(a - b) < 1e-12; }

size_t clamp_period_range(size_t value, size_t lo, size_t hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

double residual_variance(const std::vector<double>& xs,
                         const FoldedProfile& profile) {
  const size_t mod = profile.mean.size();
  if (mod == 0 || xs.empty()) return 0.0;
  double sum_sq = 0.0;
  for (size_t i = 0; i < xs.size(); ++i) {
    const double d = xs[i] - profile.mean[i % mod];
    sum_sq += d * d;
  }
  return sum_sq / static_cast<double>(xs.size());
}

double window_correlation(const std::vector<double>& xs, size_t a, size_t b,
                          size_t len) {
  double mean_a = 0.0;
  double mean_b = 0.0;
  for (size_t k = 0; k < len; ++k) {
    mean_a += xs[a + k];
    mean_b += xs[b + k];
  }
  mean_a /= static_cast<double>(len);
  mean_b /= static_cast<double>(len);

  double saa = 0.0;
  double sbb = 0.0;
  double sab = 0.0;
  for (size_t k = 0; k < len; ++k) {
    const double da = xs[a + k] - mean_a;
    const double db = xs[b + k] - mean_b;
    saa += da * da;
    sbb += db * db;
    sab += da * db;
  }
  if (!(saa > 0.0) || !(sbb > 0.0)) {
    return (saa == 0.0 && sbb == 0.0 && close_enough(mean_a, mean_b)) ? 1.0
                                                                      : 0.0;
  }
  return sab / std::sqrt(saa * sbb);
}

bool clamp_scan_range(size_t n, size_t min_period, size_t max_period,
                      size_t& lo, size_t& hi) {
  if (n < kMinSamples) return false;
  if (max_period < min_period) return false;
  const size_t upper = n / 2;
  if (upper < 1) return false;
  lo = min_period < 1 ? 1 : min_period;
  hi = clamp_period_range(max_period, 1, upper);
  lo = clamp_period_range(lo, 1, upper);
  return hi >= lo;
}

}

FoldedProfile fold_series(const std::vector<double>& xs, size_t period) {
  FoldedProfile profile;
  profile.period = period;
  if (period == 0) return profile;

  const size_t n = xs.size();
  profile.cycles = n / period;
  profile.mean.assign(period, 0.0);
  profile.band.assign(period, 0.0);

  std::vector<size_t> counts(period, 0);
  for (size_t i = 0; i < n; ++i) {
    const size_t phase = i % period;
    profile.mean[phase] += xs[i];
    ++counts[phase];
  }
  for (size_t j = 0; j < period; ++j) {
    if (counts[j] > 0) {
      profile.mean[j] /= static_cast<double>(counts[j]);
    }
  }

  std::vector<double> sq_dev(period, 0.0);
  for (size_t i = 0; i < n; ++i) {
    const size_t phase = i % period;
    const double d = xs[i] - profile.mean[phase];
    sq_dev[phase] += d * d;
  }
  for (size_t j = 0; j < period; ++j) {
    if (counts[j] > 0) {
      const double v = sq_dev[j] / static_cast<double>(counts[j]);
      profile.band[j] = std::sqrt(v > 0.0 ? v : 0.0);
    }
  }
  return profile;
}

double periodicity_score(const std::vector<double>& xs, size_t period) {
  if (period == 0 || xs.size() < 2) return 0.0;
  const double total = variance(xs, false);
  if (!(total > 0.0)) return 0.0;

  const FoldedProfile profile = fold_series(xs, period);
  const double residual = residual_variance(xs, profile);
  return clamp_unit(1.0 - residual / total);
}

double cycle_consistency(const std::vector<double>& xs, size_t period) {
  if (period == 0) return 0.0;
  const size_t cycles = xs.size() / period;
  if (cycles < 2) return 0.0;

  double sum = 0.0;
  size_t pairs = 0;
  for (size_t c = 0; c + 1 < cycles; ++c) {
    const double corr =
        window_correlation(xs, c * period, (c + 1) * period, period);
    sum += corr;
    ++pairs;
  }
  return pairs == 0 ? 0.0 : sum / static_cast<double>(pairs);
}

PeriodScan scan_periods(const std::vector<double>& xs, size_t min_period,
                        size_t max_period) {
  PeriodScan scan;
  size_t lo = 0;
  size_t hi = 0;
  if (!clamp_scan_range(xs.size(), min_period, max_period, lo, hi)) return scan;

  scan.periods.reserve(hi - lo + 1);
  scan.autocorr.reserve(hi - lo + 1);
  scan.score.reserve(hi - lo + 1);
  for (size_t p = lo; p <= hi; ++p) {
    scan.periods.push_back(p);
    scan.autocorr.push_back(autocorrelation(xs, p));
    scan.score.push_back(periodicity_score(xs, p));
  }
  return scan;
}

size_t fundamental_period(const std::vector<double>& xs, size_t period,
                          double tolerance) {
  if (period == 0) return 0;
  const double base = periodicity_score(xs, period);
  for (size_t d = 1; d < period; ++d) {
    if (period % d != 0) continue;
    if (periodicity_score(xs, d) >= base - tolerance) return d;
  }
  return period;
}

PeriodEstimate estimate_period(const std::vector<double>& xs, size_t min_period,
                               size_t max_period) {
  PeriodEstimate est;
  const PeriodScan scan = scan_periods(xs, min_period, max_period);
  if (scan.periods.empty()) return est;

  double best_ac = -std::numeric_limits<double>::infinity();
  size_t best_period = scan.periods.front();
  for (size_t i = 0; i < scan.periods.size(); ++i) {
    if (scan.autocorr[i] > best_ac) {
      best_ac = scan.autocorr[i];
      best_period = scan.periods[i];
    }
  }

  best_period = fundamental_period(xs, best_period, kHarmonicTolerance);

  est.period = best_period;
  est.strength = periodicity_score(xs, best_period);
  est.found = est.strength >= kFoundStrength;
  return est;
}

ResidualStats fold_residual(const std::vector<double>& xs,
                            const FoldedProfile& profile) {
  ResidualStats stats;
  const size_t mod = profile.mean.size();
  if (mod == 0 || xs.empty()) return stats;

  double sum_sq = 0.0;
  double max_abs = 0.0;
  size_t worst = 0;
  for (size_t i = 0; i < xs.size(); ++i) {
    const double d = xs[i] - profile.mean[i % mod];
    const double a = std::fabs(d);
    sum_sq += d * d;
    if (a > max_abs) {
      max_abs = a;
      worst = i;
    }
  }
  stats.rms = std::sqrt(sum_sq / static_cast<double>(xs.size()));
  stats.max_abs = max_abs;
  stats.worst_index = worst;
  return stats;
}

double profile_amplitude(const FoldedProfile& profile) {
  if (profile.mean.empty()) return 0.0;
  double lo = profile.mean.front();
  double hi = lo;
  for (const double m : profile.mean) {
    if (m < lo) lo = m;
    if (m > hi) hi = m;
  }
  return hi - lo;
}

size_t profile_peak_phase(const FoldedProfile& profile) {
  if (profile.mean.empty()) return 0;
  size_t best = 0;
  for (size_t j = 1; j < profile.mean.size(); ++j) {
    if (profile.mean[j] > profile.mean[best]) best = j;
  }
  return best;
}

size_t profile_trough_phase(const FoldedProfile& profile) {
  if (profile.mean.empty()) return 0;
  size_t best = 0;
  for (size_t j = 1; j < profile.mean.size(); ++j) {
    if (profile.mean[j] < profile.mean[best]) best = j;
  }
  return best;
}

std::vector<double> reconstruct(const FoldedProfile& profile, size_t count) {
  std::vector<double> out(count, 0.0);
  const size_t mod = profile.mean.size();
  if (mod == 0) return out;
  for (size_t i = 0; i < count; ++i) {
    out[i] = profile.mean[i % mod];
  }
  return out;
}

std::string render_profile(const FoldedProfile& profile) {
  char head[96];
  std::snprintf(head, sizeof(head), "period %zu cycles %zu amplitude %.6g\n",
                profile.period, profile.cycles, profile_amplitude(profile));
  std::string out = head;
  for (size_t j = 0; j < profile.mean.size(); ++j) {
    char line[96];
    std::snprintf(line, sizeof(line), "  phase %zu mean %.6g band %.6g\n", j,
                  profile.mean[j],
                  j < profile.band.size() ? profile.band[j] : 0.0);
    out += line;
  }
  return out;
}

std::vector<FoldSummary> analyze_folds(const ChannelStore& store) {
  std::vector<FoldSummary> out;
  const std::vector<uint16_t> ids = store.channels();
  for (const uint16_t id : ids) {
    const ChannelSeries* series = store.series(id);
    if (series == nullptr) continue;
    const size_t field_count = series->schema().fields.size();
    for (size_t field = 0; field < field_count; ++field) {
      const std::vector<double> values = channel_column(store, id, field);

      FoldSummary summary;
      summary.channel_id = id;
      summary.field_index = field;
      summary.samples = values.size();

      const PeriodEstimate est =
          estimate_period(values, 2, values.size());
      summary.period = est.period;
      summary.strength = est.strength;

      if (est.period > 0) {
        const FoldedProfile profile = fold_series(values, est.period);
        summary.residual_rms = fold_residual(values, profile).rms;
      }
      out.push_back(summary);
    }
  }
  return out;
}

std::string describe_fold(const FoldSummary& f) {
  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "channel %04x field %zu samples %zu period %zu strength %.4f "
                "residual-rms %.6g",
                static_cast<unsigned>(f.channel_id), f.field_index, f.samples,
                f.period, f.strength, f.residual_rms);
  return std::string(buf);
}

std::string render_folds(const std::vector<FoldSummary>& fs) {
  char head[48];
  std::snprintf(head, sizeof(head), "%zu fold(s)\n", fs.size());
  std::string out = head;
  for (const FoldSummary& f : fs) {
    out += "  ";
    out += describe_fold(f);
    out += "\n";
  }
  return out;
}

}
