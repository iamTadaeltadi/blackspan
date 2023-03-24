#ifndef BLACKSPAN_HEALTH_H
#define BLACKSPAN_HEALTH_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

struct HealthFinding {
  uint16_t channel_id = 0;
  size_t field_index = 0;
  std::string field_name;
  size_t samples = 0;
  double score = 0.0;
  bool flagged = false;
};

double volatility_score(const std::vector<double>& column);

std::vector<HealthFinding> channel_health_scan(const ChannelStore& store,
                                               double threshold);

std::string describe_health(const HealthFinding& f);

}

#endif
