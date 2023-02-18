#include <algorithm>
#include <cmath>
#include <limits>

#include "blackspan/stats.h"

namespace blackspan {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}  // namespace

double percentile(std::vector<double> values, double q) {
  if (values.empty() || std::isnan(q)) return kNaN;
  if (q < 0.0) q = 0.0;
  if (q > 100.0) q = 100.0;

  std::sort(values.begin(), values.end());
  if (values.size() == 1) return values[0];

  // Linear interpolation between the two order statistics that bracket the
  // fractional rank q/100 * (n - 1).
  const double rank = (q / 100.0) * static_cast<double>(values.size() - 1);
  const double lo_pos = std::floor(rank);
  const size_t lo_index = static_cast<size_t>(lo_pos);
  if (lo_index >= values.size() - 1) return values.back();  // rank == n-1

  const double frac = rank - lo_pos;
  const double a = values[lo_index];
  const double b = values[lo_index + 1];
  return a + frac * (b - a);
}

double median(std::vector<double> values) {
  return percentile(std::move(values), 50.0);
}

Histogram histogram(const std::vector<double>& values, size_t bucket_count,
                    std::optional<double> lo, std::optional<double> hi) {
  Histogram h;
  if (bucket_count == 0) return h;  // empty buckets; nothing to bin into
  h.buckets.assign(bucket_count, 0);

  // Resolve either supplied edge; fall back to the data extent for an
  // unsupplied one. With no data and no supplied edge, the edge is 0.
  double data_lo = 0.0;
  double data_hi = 0.0;
  if (!values.empty()) {
    data_lo = *std::min_element(values.begin(), values.end());
    data_hi = *std::max_element(values.begin(), values.end());
  }
  h.lo = lo ? *lo : data_lo;
  h.hi = hi ? *hi : data_hi;

  const bool degenerate = !(h.hi > h.lo);
  h.bucket_width =
      degenerate ? 0.0 : (h.hi - h.lo) / static_cast<double>(bucket_count);

  for (const double x : values) {
    if (x < h.lo) {
      ++h.below;
      continue;
    }
    if (x > h.hi) {
      ++h.above;
      continue;
    }
    if (degenerate) {
      // Every in-range value equals the single point lo == hi.
      ++h.buckets[0];
      continue;
    }
    size_t idx = static_cast<size_t>((x - h.lo) / h.bucket_width);
    if (idx >= bucket_count) idx = bucket_count - 1;  // x == hi lands last
    ++h.buckets[idx];
  }
  return h;
}

std::vector<double> downsample_mean(const std::vector<double>& values,
                                    size_t factor) {
  std::vector<double> out;
  if (factor == 0 || values.empty()) return out;

  out.reserve((values.size() + factor - 1) / factor);
  for (size_t i = 0; i < values.size(); i += factor) {
    const size_t end = std::min(i + factor, values.size());
    double sum = 0.0;
    for (size_t j = i; j < end; ++j) sum += values[j];
    out.push_back(sum / static_cast<double>(end - i));
  }
  return out;
}

StatsError rate_of_change(const std::vector<double>& values,
                          const std::vector<uint64_t>& timestamps,
                          std::vector<double>& out) {
  out.clear();
  if (values.size() != timestamps.size()) return StatsError::LengthMismatch;
  if (values.size() < 2) return StatsError::None;

  out.reserve(values.size() - 1);
  for (size_t i = 0; i + 1 < values.size(); ++i) {
    const double dt =
        static_cast<double>(timestamps[i + 1]) - static_cast<double>(timestamps[i]);
    if (dt == 0.0) {
      out.push_back(kNaN);  // guard the zero-interval division
    } else {
      out.push_back((values[i + 1] - values[i]) / dt);
    }
  }
  return StatsError::None;
}

}  // namespace blackspan
