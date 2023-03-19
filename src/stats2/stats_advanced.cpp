#include <algorithm>
#include <cmath>
#include <limits>

#include "blackspan/aggregate.h"
#include "blackspan/stats_advanced.h"

namespace blackspan {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Arithmetic mean of a non-empty vector.
double mean_of(const std::vector<double>& xs) {
  double sum = 0.0;
  for (const double v : xs) sum += v;
  return sum / static_cast<double>(xs.size());
}

// The p-th percentile of an already-sorted, non-empty vector under method m.
double percentile_sorted(const std::vector<double>& s, double p,
                         PercentileMethod m) {
  const size_t n = s.size();
  if (n == 1) return s[0];

  if (m == PercentileMethod::NearestRank) {
    if (p <= 0.0) return s.front();
    // Ordinal rank ceil(p/100 * N), 1-based, clamped into [1, N].
    double rankf = std::ceil((p / 100.0) * static_cast<double>(n));
    if (rankf < 1.0) rankf = 1.0;
    if (rankf > static_cast<double>(n)) rankf = static_cast<double>(n);
    const size_t idx = static_cast<size_t>(rankf) - 1;
    return s[idx];
  }

  // Linear: interpolate between the order statistics bracketing the fractional
  // rank p/100 * (n - 1).
  const double rank = (p / 100.0) * static_cast<double>(n - 1);
  const double lo_pos = std::floor(rank);
  const size_t lo_index = static_cast<size_t>(lo_pos);
  if (lo_index >= n - 1) return s.back();  // rank == n - 1
  const double frac = rank - lo_pos;
  const double a = s[lo_index];
  const double b = s[lo_index + 1];
  return a + frac * (b - a);
}

// Central moments m2/m3/m4 (population, divided by N) about the mean, for a
// non-empty vector. Returns the mean and the three moments through out-params.
void central_moments(const std::vector<double>& xs, double& mean, double& m2,
                     double& m3, double& m4) {
  mean = mean_of(xs);
  double s2 = 0.0, s3 = 0.0, s4 = 0.0;
  for (const double v : xs) {
    const double d = v - mean;
    const double d2 = d * d;
    s2 += d2;
    s3 += d2 * d;
    s4 += d2 * d2;
  }
  const double n = static_cast<double>(xs.size());
  m2 = s2 / n;
  m3 = s3 / n;
  m4 = s4 / n;
}

}  // namespace

double percentile(const std::vector<double>& xs, double p, PercentileMethod m) {
  if (xs.empty() || std::isnan(p)) return kNaN;
  if (p < 0.0) p = 0.0;
  if (p > 100.0) p = 100.0;
  std::vector<double> s = xs;  // sort a local copy; leave xs untouched
  std::sort(s.begin(), s.end());
  return percentile_sorted(s, p, m);
}

double median(const std::vector<double>& xs) {
  return percentile(xs, 50.0, PercentileMethod::Linear);
}

double quantile_iqr(const std::vector<double>& xs) {
  if (xs.empty()) return kNaN;
  // Sort once and read both quartiles off the same sorted copy.
  std::vector<double> s = xs;
  std::sort(s.begin(), s.end());
  const double q75 = percentile_sorted(s, 75.0, PercentileMethod::Linear);
  const double q25 = percentile_sorted(s, 25.0, PercentileMethod::Linear);
  return q75 - q25;
}

double variance(const std::vector<double>& xs, bool sample) {
  const size_t n = xs.size();
  if (n == 0) return 0.0;
  if (sample && n < 2) return 0.0;  // N - 1 == 0 is undefined

  const double mean = mean_of(xs);
  double sq_dev = 0.0;
  for (const double v : xs) {
    const double d = v - mean;
    sq_dev += d * d;
  }
  const double denom =
      sample ? static_cast<double>(n - 1) : static_cast<double>(n);
  return sq_dev / denom;
}

double stddev(const std::vector<double>& xs, bool sample) {
  return std::sqrt(variance(xs, sample));
}

double skewness(const std::vector<double>& xs) {
  if (xs.size() < 3) return 0.0;
  double mean, m2, m3, m4;
  central_moments(xs, mean, m2, m3, m4);
  if (!(m2 > 0.0)) return 0.0;  // zero-variance series: undefined
  return m3 / std::pow(m2, 1.5);
}

double kurtosis_excess(const std::vector<double>& xs) {
  if (xs.size() < 2) return 0.0;
  double mean, m2, m3, m4;
  central_moments(xs, mean, m2, m3, m4);
  if (!(m2 > 0.0)) return 0.0;  // zero-variance series: undefined
  return m4 / (m2 * m2) - 3.0;
}

Histogram histogram(const std::vector<double>& xs, size_t bins) {
  Histogram h;
  if (bins == 0) return h;  // no buckets to bin into
  h.counts.assign(bins, 0);
  if (xs.empty()) return h;  // all-zero counts, [0, 0] range

  h.lo = *std::min_element(xs.begin(), xs.end());
  h.hi = *std::max_element(xs.begin(), xs.end());

  const bool degenerate = !(h.hi > h.lo);  // max == min (or NaN-safe fallback)
  h.width =
      degenerate ? 0.0 : (h.hi - h.lo) / static_cast<double>(bins);

  for (const double x : xs) {
    if (degenerate) {
      ++h.counts[0];  // every value equals the single point lo == hi
      continue;
    }
    size_t idx = static_cast<size_t>((x - h.lo) / h.width);
    if (idx >= bins) idx = bins - 1;  // x == hi lands in the last bucket
    ++h.counts[idx];
  }
  return h;
}

std::vector<double> ewma(const std::vector<double>& xs, double alpha) {
  std::vector<double> out;
  if (xs.empty()) return out;
  if (!(alpha > 0.0)) alpha = std::numeric_limits<double>::min();
  if (alpha > 1.0) alpha = 1.0;

  out.reserve(xs.size());
  out.push_back(xs[0]);  // seed the recurrence with the first sample
  const double beta = 1.0 - alpha;
  for (size_t i = 1; i < xs.size(); ++i) {
    out.push_back(alpha * xs[i] + beta * out[i - 1]);
  }
  return out;
}

bool linear_fit(const std::vector<double>& xs, const std::vector<double>& ys,
                LinearFit& out) {
  out = LinearFit{};
  const size_t n = std::min(xs.size(), ys.size());
  if (n < 2) {
    if (n == 1) out.intercept = ys[0];  // mean of the single y value
    return false;
  }

  // Two-pass fit: means first, then centered cross- and self-products, which is
  // numerically steadier than accumulating raw sums of squares.
  double mx = 0.0, my = 0.0;
  for (size_t i = 0; i < n; ++i) {
    mx += xs[i];
    my += ys[i];
  }
  mx /= static_cast<double>(n);
  my /= static_cast<double>(n);

  double sxx = 0.0, sxy = 0.0, syy = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double dx = xs[i] - mx;
    const double dy = ys[i] - my;
    sxx += dx * dx;
    sxy += dx * dy;
    syy += dy * dy;
  }

  if (!(sxx > 0.0)) {          // zero x-variance: the slope is undefined
    out.intercept = my;        // best constant predictor is the mean of y
    return false;
  }

  out.slope = sxy / sxx;
  out.intercept = my - out.slope * mx;
  // r2 = explained / total; with a constant y (syy == 0) it is left at 0.
  out.r2 = (syy > 0.0) ? (sxy * sxy) / (sxx * syy) : 0.0;
  return true;
}

double mad(const std::vector<double>& xs) {
  if (xs.empty()) return 0.0;
  const double med = median(xs);
  std::vector<double> dev;
  dev.reserve(xs.size());
  for (const double v : xs) dev.push_back(std::fabs(v - med));
  return 1.4826 * median(dev);
}

std::vector<size_t> mad_outliers(const std::vector<double>& xs, double k) {
  std::vector<size_t> out;
  if (xs.empty()) return out;
  const double m = mad(xs);
  if (!(m > 0.0)) return out;  // constant series: no dispersion, no outliers
  const double med = median(xs);
  for (size_t i = 0; i < xs.size(); ++i) {
    if (std::fabs(xs[i] - med) / m > k) out.push_back(i);
  }
  return out;
}

double autocorrelation(const std::vector<double>& xs, size_t lag) {
  const size_t n = xs.size();
  if (lag == 0 || lag >= n) return 0.0;  // out-of-range lag

  const double mean = mean_of(xs);
  double denom = 0.0;
  for (const double v : xs) {
    const double d = v - mean;
    denom += d * d;
  }
  if (!(denom > 0.0)) return 0.0;  // zero-variance series

  double numer = 0.0;
  for (size_t t = 0; t + lag < n; ++t) {
    numer += (xs[t] - mean) * (xs[t + lag] - mean);
  }
  return numer / denom;
}

std::vector<double> channel_column(const ChannelStore& store,
                                   uint16_t channel_id, size_t field_index) {
  // Reuse the aggregate subsystem's block-walk, then drop the timestamps.
  const std::vector<std::pair<uint64_t, double>> pairs =
      column_with_timestamps(store, channel_id, field_index);
  std::vector<double> out;
  out.reserve(pairs.size());
  for (const auto& p : pairs) out.push_back(p.second);
  return out;
}

}  // namespace blackspan
