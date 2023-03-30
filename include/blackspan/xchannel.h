#ifndef BLACKSPAN_XCHANNEL_H
#define BLACKSPAN_XCHANNEL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/channel.h"
#include "blackspan/join.h"

namespace blackspan {

struct PairStats {
  size_t n = 0;
  double covariance = 0.0;
  double correlation = 0.0;
  double slope = 0.0;
  double intercept = 0.0;
  double r2 = 0.0;
  double rmse = 0.0;
};

PairStats pair_stats_aligned(const std::vector<double>& xs,
                             const std::vector<double>& ys);

PairStats pair_stats(const std::vector<std::pair<uint64_t, double>>& left,
                     const std::vector<std::pair<uint64_t, double>>& right,
                     JoinKind align, uint64_t tolerance_ns);

double aligned_correlation(
    const std::vector<std::pair<uint64_t, double>>& left,
    const std::vector<std::pair<uint64_t, double>>& right, JoinKind align,
    uint64_t tolerance_ns);

struct ChannelFieldRef {
  uint16_t channel_id = 0;
  size_t field_index = 0;
};

std::string describe_ref(const ChannelFieldRef& ref);

struct PairResult {
  ChannelFieldRef a;
  ChannelFieldRef b;
  PairStats stats;
};

std::vector<ChannelFieldRef> enumerate_fields(const ChannelStore& store);

std::vector<PairResult> correlation_matrix_with(const ChannelStore& store,
                                                JoinKind align,
                                                uint64_t tolerance_ns);

std::vector<PairResult> correlation_matrix(const ChannelStore& store,
                                           uint64_t tolerance_ns);

std::vector<size_t> rank_pairs(const std::vector<PairResult>& pairs);

std::vector<PairResult> top_correlated(const std::vector<PairResult>& pairs,
                                       size_t k);

std::vector<PairResult> filter_pairs(const std::vector<PairResult>& pairs,
                                     double min_abs_correlation);

std::string describe_pair(const PairResult& p);

std::string render_matrix(const std::vector<PairResult>& pairs);

struct LagScore {
  int64_t lag = 0;
  size_t overlap = 0;
  double correlation = 0.0;
};

std::vector<LagScore> lag_profile(
    const std::vector<std::pair<uint64_t, double>>& left,
    const std::vector<std::pair<uint64_t, double>>& right, JoinKind align,
    uint64_t tolerance_ns, int64_t max_lag);

LagScore best_lag(const std::vector<std::pair<uint64_t, double>>& left,
                  const std::vector<std::pair<uint64_t, double>>& right,
                  JoinKind align, uint64_t tolerance_ns, int64_t max_lag);

struct MatrixSummary {
  size_t pair_count = 0;
  size_t evaluated = 0;
  double max_abs_correlation = 0.0;
  double mean_abs_correlation = 0.0;
  size_t strongest_index = 0;
  bool has_strongest = false;
};

MatrixSummary summarize_matrix(const std::vector<PairResult>& pairs);

std::string describe_matrix_summary(const MatrixSummary& s);

}

#endif
