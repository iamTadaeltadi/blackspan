#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/aggregate.h"
#include "blackspan/stats_advanced.h"
#include "blackspan/trend.h"

namespace blackspan {

namespace {

double signed_delta(uint64_t ts, uint64_t base) {
  if (ts >= base) return static_cast<double>(ts - base);
  return -static_cast<double>(base - ts);
}

struct PrefixSums {
  std::vector<double> sum_v;
  std::vector<double> sum_vv;

  explicit PrefixSums(const std::vector<std::pair<uint64_t, double>>& series) {
    const size_t n = series.size();
    sum_v.assign(n + 1, 0.0);
    sum_vv.assign(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) {
      const double v = series[i].second;
      sum_v[i + 1] = sum_v[i] + v;
      sum_vv[i + 1] = sum_vv[i] + v * v;
    }
  }

  double mean(size_t begin, size_t end) const {
    if (end <= begin) return 0.0;
    return (sum_v[end] - sum_v[begin]) / static_cast<double>(end - begin);
  }

  double residual_sum_squares(size_t begin, size_t end) const {
    if (end <= begin) return 0.0;
    const double span = static_cast<double>(end - begin);
    const double s = sum_v[end] - sum_v[begin];
    const double raw = (sum_vv[end] - sum_vv[begin]) - s * s / span;
    return raw > 0.0 ? raw : 0.0;
  }
};

double range_slope(const std::vector<std::pair<uint64_t, double>>& series,
                   size_t begin, size_t end) {
  if (end < begin || end - begin < 2) return 0.0;
  const uint64_t anchor = series[begin].first;
  std::vector<double> xs;
  std::vector<double> ys;
  xs.reserve(end - begin);
  ys.reserve(end - begin);
  for (size_t i = begin; i < end; ++i) {
    xs.push_back(signed_delta(series[i].first, anchor));
    ys.push_back(series[i].second);
  }
  LinearFit fit;
  linear_fit(xs, ys, fit);
  return fit.slope;
}

std::vector<size_t> binary_segmentation(
    const std::vector<std::pair<uint64_t, double>>& series,
    const PrefixSums& prefix, double threshold, size_t run) {
  std::vector<size_t> breaks;
  const size_t n = series.size();
  if (n < 2 * run) return breaks;

  std::vector<std::pair<size_t, size_t>> pending;
  pending.emplace_back(0, n);

  while (!pending.empty()) {
    const std::pair<size_t, size_t> region = pending.back();
    pending.pop_back();
    const size_t begin = region.first;
    const size_t end = region.second;
    if (end - begin < 2 * run) continue;

    const double base_cost = prefix.residual_sum_squares(begin, end);
    size_t best_split = 0;
    double best_gain = 0.0;
    bool found = false;
    for (size_t split = begin + run; split + run <= end; ++split) {
      const double gain = base_cost - prefix.residual_sum_squares(begin, split) -
                          prefix.residual_sum_squares(split, end);
      if (gain > best_gain) {
        best_gain = gain;
        best_split = split;
        found = true;
      }
    }
    if (!found) continue;

    const double left_mean = prefix.mean(begin, best_split);
    const double right_mean = prefix.mean(best_split, end);
    if (std::fabs(right_mean - left_mean) <= threshold) continue;

    breaks.push_back(best_split);
    pending.emplace_back(begin, best_split);
    pending.emplace_back(best_split, end);
  }

  std::sort(breaks.begin(), breaks.end());
  return breaks;
}

void prune_small_steps(std::vector<size_t>& bounds, const PrefixSums& prefix,
                       double threshold) {
  bool changed = true;
  while (bounds.size() > 2 && changed) {
    changed = false;
    size_t victim = 0;
    double smallest_gap = -1.0;
    for (size_t j = 1; j + 1 < bounds.size(); ++j) {
      const double left_mean = prefix.mean(bounds[j - 1], bounds[j]);
      const double right_mean = prefix.mean(bounds[j], bounds[j + 1]);
      const double gap = std::fabs(right_mean - left_mean);
      if (smallest_gap < 0.0 || gap < smallest_gap) {
        smallest_gap = gap;
        victim = j;
      }
    }
    if (victim != 0 && smallest_gap <= threshold) {
      bounds.erase(bounds.begin() + static_cast<std::ptrdiff_t>(victim));
      changed = true;
    }
  }
}

double robust_spread(const std::vector<double>& values) {
  const double dispersion = mad(values);
  if (dispersion > 0.0) return dispersion;
  if (values.empty()) return 0.0;
  double lo = values.front();
  double hi = values.front();
  for (const double v : values) {
    lo = v < lo ? v : lo;
    hi = v > hi ? v : hi;
  }
  const double span = hi - lo;
  return span > 0.0 ? span / 8.0 : 0.0;
}

}

TrendLine fit_trend(const std::vector<std::pair<uint64_t, double>>& series) {
  TrendLine out;
  if (series.empty()) return out;

  out.t0 = series.front().first;
  std::vector<double> xs;
  std::vector<double> ys;
  xs.reserve(series.size());
  ys.reserve(series.size());
  for (const auto& sample : series) {
    xs.push_back(signed_delta(sample.first, out.t0));
    ys.push_back(sample.second);
  }

  LinearFit fit;
  linear_fit(xs, ys, fit);
  out.slope = fit.slope;
  out.intercept = fit.intercept;
  out.r2 = fit.r2;
  return out;
}

double drift_rate(const std::vector<std::pair<uint64_t, double>>& series) {
  return fit_trend(series).slope;
}

std::vector<Segment> segment_by_changepoints(
    const std::vector<std::pair<uint64_t, double>>& series, double min_step,
    size_t min_run) {
  std::vector<Segment> out;
  const size_t n = series.size();
  if (n == 0) return out;

  const size_t run = min_run < 1 ? 1 : min_run;
  const double threshold = std::fabs(min_step);
  const PrefixSums prefix(series);

  std::vector<size_t> bounds;
  bounds.push_back(0);
  const std::vector<size_t> breaks =
      binary_segmentation(series, prefix, threshold, run);
  for (const size_t b : breaks) bounds.push_back(b);
  bounds.push_back(n);

  prune_small_steps(bounds, prefix, threshold);

  out.reserve(bounds.size() - 1);
  for (size_t j = 0; j + 1 < bounds.size(); ++j) {
    Segment seg;
    seg.begin_index = bounds[j];
    seg.end_index = bounds[j + 1];
    seg.mean = prefix.mean(seg.begin_index, seg.end_index);
    seg.slope = range_slope(series, seg.begin_index, seg.end_index);
    out.push_back(seg);
  }
  return out;
}

std::vector<MonotonicRun> monotonic_runs(
    const std::vector<std::pair<uint64_t, double>>& series) {
  std::vector<MonotonicRun> out;
  const size_t n = series.size();
  if (n < 2) return out;

  size_t start = 0;
  while (start + 1 < n) {
    const bool non_decreasing = series[start + 1].second >= series[start].second;
    size_t last = start + 1;
    while (last + 1 < n) {
      const double current = series[last].second;
      const double next = series[last + 1].second;
      const bool consistent =
          non_decreasing ? (next >= current) : (next <= current);
      if (!consistent) break;
      ++last;
    }
    MonotonicRun run;
    run.begin_index = start;
    run.end_index = last;
    run.increasing = non_decreasing;
    out.push_back(run);
    start = last;
  }
  return out;
}

std::vector<TrendSummary> analyze_trends(const ChannelStore& store) {
  std::vector<TrendSummary> out;
  const std::vector<uint16_t> ids = store.channels();
  for (const uint16_t id : ids) {
    const ChannelSeries* series = store.series(id);
    if (series == nullptr) continue;
    const size_t field_total = series->schema().fields.size();
    for (size_t field = 0; field < field_total; ++field) {
      const std::vector<std::pair<uint64_t, double>> column =
          column_with_timestamps(store, id, field);

      TrendSummary summary;
      summary.channel_id = id;
      summary.field_index = field;
      summary.samples = column.size();

      const TrendLine line = fit_trend(column);
      summary.slope = line.slope;
      summary.r2 = line.r2;
      summary.drift_rate = line.slope;

      std::vector<double> values;
      values.reserve(column.size());
      for (const auto& sample : column) values.push_back(sample.second);

      const double spread = robust_spread(values);
      const double step = spread > 0.0 ? 4.0 * spread : 1.0;
      size_t run = column.size() / 16;
      if (run < 2) run = 2;

      const std::vector<Segment> segments =
          segment_by_changepoints(column, step, run);
      summary.changepoints = segments.empty() ? 0 : segments.size() - 1;
      summary.monotonic_runs = monotonic_runs(column).size();

      out.push_back(summary);
    }
  }
  return out;
}

std::string describe_trend(const TrendSummary& t) {
  const char* direction;
  if (t.samples < 2) {
    direction = "insufficient";
  } else if (t.r2 < 0.05) {
    direction = "flat";
  } else if (t.drift_rate > 0.0) {
    direction = "rising";
  } else {
    direction = "falling";
  }

  const char* confidence;
  if (t.samples < 2) {
    confidence = "none";
  } else if (t.r2 >= 0.9) {
    confidence = "strong";
  } else if (t.r2 >= 0.5) {
    confidence = "moderate";
  } else if (t.r2 >= 0.05) {
    confidence = "weak";
  } else {
    confidence = "noise";
  }

  const char* stability;
  if (t.changepoints == 0) {
    stability = "stable";
  } else if (t.changepoints == 1) {
    stability = "one-step";
  } else {
    stability = "multi-step";
  }

  const char* shape;
  if (t.monotonic_runs <= 1) {
    shape = "monotonic";
  } else if (t.monotonic_runs == 2) {
    shape = "single-turn";
  } else {
    shape = "oscillating";
  }

  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "channel=%u field=%zu n=%zu slope=%.6g r2=%.4f drift=%.6g "
                "cps=%zu runs=%zu [%s %s %s %s]",
                static_cast<unsigned>(t.channel_id), t.field_index, t.samples,
                t.slope, t.r2, t.drift_rate, t.changepoints, t.monotonic_runs,
                direction, confidence, stability, shape);
  return std::string(buf);
}

}
