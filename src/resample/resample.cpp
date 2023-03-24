#include "blackspan/resample.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace blackspan {
namespace {

using Sample = std::pair<uint64_t, double>;

bool by_ts(const Sample& a, const Sample& b) { return a.first < b.first; }

double nan_value() { return std::numeric_limits<double>::quiet_NaN(); }

std::vector<Sample> channel_column(const ChannelStore& store, uint16_t channel_id,
                                   size_t field_index) {
  std::vector<Sample> out;
  const ChannelSeries* series = store.series(channel_id);
  if (series == nullptr) return out;
  if (field_index >= series->schema().fields.size()) return out;

  size_t global = 0;
  for (size_t b = 0; b < series->block_count(); ++b) {
    const uint64_t base_ts = series->block_base_ts(b);
    const uint16_t n = series->block_sample_count(b);
    for (uint16_t i = 0; i < n; ++i, ++global) {
      const std::optional<SampleView> view = series->view(global);
      if (!view.has_value()) continue;
      const std::optional<double> value = view->get_as_double(field_index);
      if (!value.has_value()) continue;
      out.emplace_back(base_ts, value.value());
    }
  }
  return out;
}

double sample_at(const std::vector<Sample>& samples, uint64_t g,
                 ResampleMethod method, uint64_t max_gap) {
  const uint64_t first_ts = samples.front().first;
  const uint64_t last_ts = samples.back().first;
  if (g < first_ts || g > last_ts) return nan_value();

  const auto ub = std::upper_bound(
      samples.begin(), samples.end(), g,
      [](uint64_t v, const Sample& e) { return v < e.first; });
  const size_t hi = static_cast<size_t>(ub - samples.begin());

  const size_t prev_idx = hi - 1;
  const uint64_t prev_ts = samples[prev_idx].first;
  const double prev_val = samples[prev_idx].second;
  const bool has_next = hi < samples.size();

  auto within_gap = [&](uint64_t gap) {
    return max_gap == 0 || gap <= max_gap;
  };

  switch (method) {
    case ResampleMethod::PreviousHold: {
      const uint64_t gap = g - prev_ts;
      return within_gap(gap) ? prev_val : nan_value();
    }
    case ResampleMethod::Nearest: {
      if (!has_next) {

        return within_gap(g - prev_ts) ? prev_val : nan_value();
      }
      const uint64_t next_ts = samples[hi].first;
      const double next_val = samples[hi].second;
      const uint64_t d_prev = g - prev_ts;
      const uint64_t d_next = next_ts - g;

      if (d_prev <= d_next) {
        return within_gap(d_prev) ? prev_val : nan_value();
      }
      return within_gap(d_next) ? next_val : nan_value();
    }
    case ResampleMethod::Linear: {

      if (!has_next || prev_ts == g) return prev_val;
      const uint64_t next_ts = samples[hi].first;
      const double next_val = samples[hi].second;
      const uint64_t width = next_ts - prev_ts;
      if (!within_gap(width)) return nan_value();
      if (width == 0) return next_val;
      const double frac = static_cast<double>(g - prev_ts) /
                          static_cast<double>(width);
      return prev_val + (next_val - prev_val) * frac;
    }
  }
  return nan_value();
}

ResampledSeries resample_sorted(const std::vector<Sample>& samples,
                                const ResamplePlan& plan) {
  ResampledSeries out;
  const ResampleGrid& grid = plan.grid;
  out.ts.reserve(grid.count);
  out.values.reserve(grid.count);

  for (size_t i = 0; i < grid.count; ++i) {
    const uint64_t g = grid.at(i);
    out.ts.push_back(g);
    double v;
    if (samples.empty()) {
      v = nan_value();
    } else {
      v = sample_at(samples, g, plan.method, plan.max_gap_ns);
    }
    if (std::isnan(v)) {
      ++out.nan;
    } else {
      ++out.defined;
    }
    out.values.push_back(v);
  }
  return out;
}

}

const char* resample_method_name(ResampleMethod m) {
  switch (m) {
    case ResampleMethod::Nearest: return "nearest";
    case ResampleMethod::PreviousHold: return "hold";
    case ResampleMethod::Linear: return "linear";
  }
  return "unknown";
}

bool parse_resample_method(const std::string& s, ResampleMethod& out) {
  if (s == "nearest") { out = ResampleMethod::Nearest; return true; }
  if (s == "hold") { out = ResampleMethod::PreviousHold; return true; }
  if (s == "linear") { out = ResampleMethod::Linear; return true; }
  return false;
}

ResampleGrid grid_from_series(const std::vector<std::pair<uint64_t, double>>& in,
                              uint64_t step) {
  ResampleGrid grid;
  if (in.empty() || step == 0) return grid;

  uint64_t lo = in.front().first;
  uint64_t hi = in.front().first;
  for (const Sample& s : in) {
    if (s.first < lo) lo = s.first;
    if (s.first > hi) hi = s.first;
  }

  grid.start = lo;
  grid.step = step;

  grid.count = static_cast<size_t>((hi - lo) / step) + 1;
  return grid;
}

ResampledSeries resample_series(const std::vector<std::pair<uint64_t, double>>& in,
                                const ResamplePlan& plan) {

  if (std::is_sorted(in.begin(), in.end(), by_ts)) {
    return resample_sorted(in, plan);
  }
  std::vector<Sample> sorted = in;
  std::stable_sort(sorted.begin(), sorted.end(), by_ts);
  return resample_sorted(sorted, plan);
}

ResampledSeries resample_channel(const ChannelStore& store, uint16_t channel_id,
                                 size_t field_index, const ResamplePlan& plan) {
  return resample_series(channel_column(store, channel_id, field_index), plan);
}

ResampledSeries resample_channel(const ChannelStore& store, uint16_t channel_id,
                                 const std::string& field_name,
                                 const ResamplePlan& plan) {
  const ChannelSeries* series = store.series(channel_id);
  if (series == nullptr) {
    return resample_series({}, plan);
  }
  const std::optional<size_t> field_index =
      series->schema().field_index(field_name);
  if (!field_index.has_value()) {
    return resample_series({}, plan);
  }
  return resample_series(channel_column(store, channel_id, *field_index), plan);
}

}
