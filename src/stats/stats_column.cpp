#include <cmath>

#include "blackspan/stats.h"

namespace blackspan {

const char* stats_error_str(StatsError e) {
  switch (e) {
    case StatsError::None:
      return "ok";
    case StatsError::UnknownChannel:
      return "unknown-channel";
    case StatsError::BadField:
      return "bad-field";
    case StatsError::EmptyColumn:
      return "empty-column";
    case StatsError::LengthMismatch:
      return "length-mismatch";
  }
  return "unknown";
}

ColumnStats compute_stats(const std::vector<double>& values) {
  ColumnStats s;
  if (values.empty()) return s;

  double sum = 0.0;
  double lo = values[0];
  double hi = values[0];
  for (const double v : values) {
    sum += v;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }

  const double n = static_cast<double>(values.size());
  const double mean = sum / n;

  double sq_dev = 0.0;
  for (const double v : values) {
    const double d = v - mean;
    sq_dev += d * d;
  }

  s.count = values.size();
  s.min = lo;
  s.max = hi;
  s.mean = mean;
  s.sum = sum;
  s.stddev = std::sqrt(sq_dev / n);
  s.valid = true;
  return s;
}

StatsError compute_column_stats(const ChannelStore& store, uint16_t channel_id,
                                size_t field_index, ColumnStats& out) {
  out = ColumnStats{};

  const ChannelSeries* series = store.series(channel_id);
  if (series == nullptr) return StatsError::UnknownChannel;
  if (field_index >= series->schema().fields.size()) {
    return StatsError::BadField;
  }

  const std::vector<double> column =
      store.column_as_doubles(channel_id, field_index);
  if (column.empty()) return StatsError::EmptyColumn;

  out = compute_stats(column);
  return StatsError::None;
}

}
