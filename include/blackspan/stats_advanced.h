#ifndef BLACKSPAN_STATS_ADVANCED_H
#define BLACKSPAN_STATS_ADVANCED_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

enum class PercentileMethod : uint8_t { NearestRank = 0, Linear = 1 };

double percentile(const std::vector<double>& xs, double p, PercentileMethod m);

double median(const std::vector<double>& xs);

double quantile_iqr(const std::vector<double>& xs);

double variance(const std::vector<double>& xs, bool sample);

double stddev(const std::vector<double>& xs, bool sample);

double skewness(const std::vector<double>& xs);

double kurtosis_excess(const std::vector<double>& xs);

struct Histogram {
  std::vector<size_t> counts;
  double lo = 0.0;
  double hi = 0.0;
  double width = 0.0;
};
Histogram histogram(const std::vector<double>& xs, size_t bins);

std::vector<double> ewma(const std::vector<double>& xs, double alpha);

struct LinearFit {
  double slope = 0.0;
  double intercept = 0.0;
  double r2 = 0.0;
};
bool linear_fit(const std::vector<double>& xs, const std::vector<double>& ys,
                LinearFit& out);

double mad(const std::vector<double>& xs);

std::vector<size_t> mad_outliers(const std::vector<double>& xs, double k);

double autocorrelation(const std::vector<double>& xs, size_t lag);

std::vector<double> channel_column(const ChannelStore& store, uint16_t channel_id,
                                   size_t field_index);

}

#endif
