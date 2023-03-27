#ifndef BLACKSPAN_TREND_H
#define BLACKSPAN_TREND_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

struct TrendLine {
  double slope = 0.0;
  double intercept = 0.0;
  double r2 = 0.0;
  uint64_t t0 = 0;
};

TrendLine fit_trend(const std::vector<std::pair<uint64_t, double>>& series);

struct Segment {
  size_t begin_index = 0;
  size_t end_index = 0;
  double slope = 0.0;
  double mean = 0.0;
};

std::vector<Segment> segment_by_changepoints(
    const std::vector<std::pair<uint64_t, double>>& series, double min_step,
    size_t min_run);

struct MonotonicRun {
  size_t begin_index = 0;
  size_t end_index = 0;
  bool increasing = true;
};

std::vector<MonotonicRun> monotonic_runs(
    const std::vector<std::pair<uint64_t, double>>& series);

double drift_rate(const std::vector<std::pair<uint64_t, double>>& series);

struct TrendSummary {
  uint16_t channel_id = 0;
  size_t field_index = 0;
  size_t samples = 0;
  double slope = 0.0;
  double r2 = 0.0;
  double drift_rate = 0.0;
  size_t changepoints = 0;
  size_t monotonic_runs = 0;
};

std::vector<TrendSummary> analyze_trends(const ChannelStore& store);

std::string describe_trend(const TrendSummary& t);

}

#endif
