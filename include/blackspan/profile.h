#ifndef BLACKSPAN_PROFILE_H
#define BLACKSPAN_PROFILE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

struct ActivityProfile {
  uint16_t channel_id = 0;
  size_t field_index = 0;
  std::string field_name = {};
  size_t samples = 0;
  double mean = 0.0;
  std::vector<float> level;
};

ActivityProfile field_activity(const ChannelStore& store, uint16_t channel_id,
                               size_t field_index);

std::vector<ActivityProfile> channel_activity(const ChannelStore& store);

std::string describe_activity(const ActivityProfile& p);

}

#endif
