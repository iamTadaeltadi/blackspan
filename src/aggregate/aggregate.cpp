#include <algorithm>
#include <cstdio>
#include <limits>

#include "blackspan/aggregate.h"

namespace blackspan {
namespace {

// Reduces one window's values with the given function. `values` is non-empty.
double reduce(const std::vector<double>& values, AggFn fn) {
  switch (fn) {
    case AggFn::Count:
      return static_cast<double>(values.size());
    case AggFn::First:
      return values.front();
    case AggFn::Last:
      return values.back();
    case AggFn::Min: {
      double m = values.front();
      for (double v : values) m = v < m ? v : m;
      return m;
    }
    case AggFn::Max: {
      double m = values.front();
      for (double v : values) m = v > m ? v : m;
      return m;
    }
    case AggFn::Sum: {
      double s = 0.0;
      for (double v : values) s += v;
      return s;
    }
    case AggFn::Mean: {
      double s = 0.0;
      for (double v : values) s += v;
      return s / static_cast<double>(values.size());
    }
  }
  return 0.0;
}

}  // namespace

const char* agg_fn_name(AggFn f) {
  switch (f) {
    case AggFn::Mean: return "mean";
    case AggFn::Min: return "min";
    case AggFn::Max: return "max";
    case AggFn::First: return "first";
    case AggFn::Last: return "last";
    case AggFn::Count: return "count";
    case AggFn::Sum: return "sum";
  }
  return "unknown";
}

bool parse_agg_fn(const std::string& s, AggFn& out) {
  if (s == "mean") { out = AggFn::Mean; return true; }
  if (s == "min") { out = AggFn::Min; return true; }
  if (s == "max") { out = AggFn::Max; return true; }
  if (s == "first") { out = AggFn::First; return true; }
  if (s == "last") { out = AggFn::Last; return true; }
  if (s == "count") { out = AggFn::Count; return true; }
  if (s == "sum") { out = AggFn::Sum; return true; }
  return false;
}

std::vector<std::pair<uint64_t, double>> column_with_timestamps(
    const ChannelStore& store, uint16_t channel_id, size_t field_index) {
  std::vector<std::pair<uint64_t, double>> out;
  const ChannelSeries* series = store.series(channel_id);
  if (series == nullptr) return out;
  if (field_index >= series->schema().fields.size()) return out;

  // Walk the series block by block so each sample can be paired with its block's
  // base timestamp, using a running global index for the typed view.
  size_t global = 0;
  for (size_t b = 0; b < series->block_count(); ++b) {
    uint64_t base_ts = series->block_base_ts(b);
    uint16_t n = series->block_sample_count(b);
    for (uint16_t i = 0; i < n; ++i, ++global) {
      auto view = series->view(global);
      if (!view.has_value()) continue;
      auto value = view->get_as_double(field_index);
      if (!value.has_value()) continue;
      out.emplace_back(base_ts, value.value());
    }
  }
  return out;
}

std::vector<AggBucket> aggregate_series(
    const std::vector<std::pair<uint64_t, double>>& samples, uint64_t window_ns,
    AggFn fn) {
  std::vector<AggBucket> out;
  if (window_ns == 0 || samples.empty()) return out;

  // Sort by timestamp so windows are contiguous even if the caller supplied
  // unordered pairs. A stable sort keeps equal-timestamp samples in input order,
  // which First/Last depend on.
  std::vector<std::pair<uint64_t, double>> sorted = samples;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const std::pair<uint64_t, double>& a,
                      const std::pair<uint64_t, double>& b) {
                     return a.first < b.first;
                   });

  const uint64_t t0 = sorted.front().first;
  uint64_t current_window = 0;
  bool have_window = false;
  std::vector<double> window_values;

  auto flush = [&]() {
    if (window_values.empty()) return;
    AggBucket bucket;
    bucket.window_start = t0 + current_window * window_ns;
    bucket.count = window_values.size();
    bucket.value = reduce(window_values, fn);
    out.push_back(bucket);
    window_values.clear();
  };

  for (const auto& s : sorted) {
    uint64_t w = (s.first - t0) / window_ns;
    if (!have_window) {
      current_window = w;
      have_window = true;
    } else if (w != current_window) {
      flush();
      current_window = w;
    }
    window_values.push_back(s.second);
  }
  flush();
  return out;
}

std::vector<AggBucket> aggregate_channel(const ChannelStore& store,
                                         uint16_t channel_id,
                                         size_t field_index, uint64_t window_ns,
                                         AggFn fn) {
  return aggregate_series(
      column_with_timestamps(store, channel_id, field_index), window_ns, fn);
}

std::string render_buckets(const std::vector<AggBucket>& buckets, AggFn fn) {
  if (buckets.empty()) return "0 windows\n";
  std::string out;
  char head[64];
  std::snprintf(head, sizeof(head), "%zu windows (%s)\n", buckets.size(),
                agg_fn_name(fn));
  out += head;
  for (const AggBucket& b : buckets) {
    char line[96];
    std::snprintf(line, sizeof(line), "  ts=%llu n=%zu %s=%.6g\n",
                  static_cast<unsigned long long>(b.window_start), b.count,
                  agg_fn_name(fn), b.value);
    out += line;
  }
  return out;
}

}  // namespace blackspan
