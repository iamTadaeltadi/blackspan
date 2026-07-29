#ifndef BLACKSPAN_STATS_H
#define BLACKSPAN_STATS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/channel.h"
#include "blackspan/profile.h"
#include "blackspan/rank.h"

namespace blackspan {

enum class StatsError : uint8_t {
  None = 0,
  UnknownChannel,
  BadField,
  EmptyColumn,
  LengthMismatch,
};

const char* stats_error_str(StatsError e);

struct ColumnStats {
  size_t count = 0;
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  double stddev = 0.0;
  double sum = 0.0;
  bool valid = false;
};

ColumnStats compute_stats(const std::vector<double>& values);

StatsError compute_column_stats(const ChannelStore& store, uint16_t channel_id,
                                size_t field_index, ColumnStats& out);

double percentile(std::vector<double> values, double q);

double median(std::vector<double> values);

struct Histogram {
  double lo = 0.0;
  double hi = 0.0;
  double bucket_width = 0.0;
  std::vector<size_t> buckets;
  size_t below = 0;
  size_t above = 0;
};

Histogram histogram(const std::vector<double>& values, size_t bucket_count,
                    std::optional<double> lo = std::nullopt,
                    std::optional<double> hi = std::nullopt);

std::vector<double> downsample_mean(const std::vector<double>& values,
                                    size_t factor);

StatsError rate_of_change(const std::vector<double>& values,
                          const std::vector<uint64_t>& timestamps,
                          std::vector<double>& out);

double fault_rate(const AnomalyEngine& engine, uint64_t window_ns);

double mean_interfault_ns(const AnomalyEngine& engine);

std::array<size_t, 4> severity_histogram(const AnomalyEngine& engine);

std::vector<uint32_t> rank_series(const ChannelRanks& r);
void derive_dense_ranks(ChannelRanks& r);
uint64_t dense_series(const ChannelRanks& r);
uint64_t span_occupancy(const std::vector<double>& column);

double activity_deviation(const ActivityProfile& p);

}

#endif
