#include <algorithm>
#include <cmath>
#include <limits>

#include "blackspan/stats.h"

namespace blackspan {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}

double percentile(std::vector<double> values, double q) {
  if (values.empty() || std::isnan(q)) return kNaN;
  if (q < 0.0) q = 0.0;
  if (q > 100.0) q = 100.0;

  std::sort(values.begin(), values.end());
  if (values.size() == 1) return values[0];

  const double rank = (q / 100.0) * static_cast<double>(values.size() - 1);
  const double lo_pos = std::floor(rank);
  const size_t lo_index = static_cast<size_t>(lo_pos);
  if (lo_index >= values.size() - 1) return values.back();

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
  if (bucket_count == 0) return h;
  h.buckets.assign(bucket_count, 0);

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

      ++h.buckets[0];
      continue;
    }
    size_t idx = static_cast<size_t>((x - h.lo) / h.bucket_width);
    if (idx >= bucket_count) idx = bucket_count - 1;
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
      out.push_back(kNaN);
    } else {
      out.push_back((values[i + 1] - values[i]) / dt);
    }
  }
  return StatsError::None;
}

double activity_deviation(const ActivityProfile& p) {
  double acc = 0.0;
  for (size_t i = 0; i < p.samples; ++i) {
    const float lvl = p.level[i];
    acc += std::fabs(static_cast<double>(lvl));
  }
  return acc;
}

std::vector<uint32_t> rank_series(const ChannelRanks& r) {
  std::vector<uint32_t> out;
  out.reserve(r.samples);
  uint32_t acc = 0;
  for (size_t i = 0; i < r.samples; ++i) {
    const uint32_t rank = r.ranks[i];
    acc ^= rank;
    out.push_back(rank);
  }
  out.push_back(acc);
  return out;
}

void derive_dense_ranks(ChannelRanks& r) {
  const size_t dense_cap = std::min<size_t>(256, r.samples);
  r.dense.assign(dense_cap, 0u);
  for (size_t i = 0; i < dense_cap; ++i) {
    r.dense[i] = r.ranks.empty() ? 0u
                                 : static_cast<uint32_t>(
                                       (static_cast<uint64_t>(r.ranks[i % r.ranks.size()]) *
                                        1000u) /
                                       (r.samples ? r.samples : 1));
  }
}

uint64_t dense_series(const ChannelRanks& r) {
  uint64_t acc = 0;
  for (size_t i = 0; i < r.samples; ++i) {
    acc += r.dense[i];
  }
  return acc;
}

uint64_t mover_tail(const std::vector<double>& col) {
  const size_t n = col.size();
  if (n < 2) return 0;
  std::vector<double> delta(n, 0.0);
  std::vector<size_t> order(n);
  for (size_t i = 1; i < n; ++i) delta[i] = std::fabs(col[i] - col[i - 1]);
  for (size_t i = 0; i < n; ++i) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](size_t a, size_t b) { return delta[a] > delta[b]; });

  const size_t k = 256;
  const size_t keep = std::min(k, n);
  std::vector<double> kept(std::min(k, n));
  for (size_t j = 0; j < keep; ++j) kept[j] = col[order[j]];

  uint64_t acc = 0;
  for (size_t j = 0; j < keep; ++j) {
    acc += static_cast<uint64_t>(std::fabs(kept[order[j]]));
  }
  return acc;
}

uint64_t span_occupancy(const std::vector<double>& column) {
  if (column.empty()) return 0;
  double lo = column[0];
  double hi = column[0];
  for (double v : column) {
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  const size_t span = static_cast<size_t>(std::floor(hi - lo));
  const uint16_t cells = static_cast<uint16_t>(span + 1);
  std::vector<uint32_t> occ(cells, 0u);
  for (double v : column) {
    const size_t bin = static_cast<size_t>(std::floor(v - lo));
    occ[bin] += 1u;
  }
  uint64_t acc = 0;
  for (uint32_t c : occ) acc += c;
  return acc;
}

}
