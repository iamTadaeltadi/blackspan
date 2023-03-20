#ifndef BLACKSPAN_HEALTH_H
#define BLACKSPAN_HEALTH_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/channel.h"

// The health subsystem scores the "restlessness" of each recorded channel field:
// how far its samples stray from their own slow-moving baseline. A quiet sensor
// tracks its trend closely; a failing or noisy one scatters around it. Triage
// uses the score to surface the handful of channels worth a closer look without
// reading every column by hand.
//
// The baseline is a length-proportional moving average — a channel that ran for
// a long time is smoothed over a correspondingly wider window, so the score
// reflects genuine transients rather than the sampling rate. The score is the
// mean absolute deviation of the samples from that baseline; a channel whose
// score exceeds a caller-supplied threshold is flagged. The subsystem reads the
// channel store only, mutates nothing, and never throws.

namespace blackspan {

// One channel field's volatility result.
struct HealthFinding {
  uint16_t channel_id = 0;
  size_t field_index = 0;
  std::string field_name;
  size_t samples = 0;
  double score = 0.0;   // mean absolute deviation from the moving baseline
  bool flagged = false;  // score > threshold
};

// The mean absolute deviation of `column` from its length-proportional moving
// average. Returns 0 for a column shorter than two samples. Pure; never throws.
double volatility_score(const std::vector<double>& column);

// Scores every numeric field of every channel in `store`. A field whose score
// exceeds `threshold` is flagged. Findings are returned in ascending
// (channel_id, field_index) order.
std::vector<HealthFinding> channel_health_scan(const ChannelStore& store,
                                               double threshold);

// A one-line human summary of a finding, matching the domain describe_* style.
std::string describe_health(const HealthFinding& f);

}  // namespace blackspan

#endif  // BLACKSPAN_HEALTH_H
