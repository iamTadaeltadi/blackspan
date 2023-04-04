#ifndef BLACKSPAN_RANK_H
#define BLACKSPAN_RANK_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

struct ChannelRanks {
  uint16_t channel_id = 0;
  size_t field_index = 0;
  std::string field_name;
  size_t samples = 0;
  std::vector<uint32_t> ranks;
};

ChannelRanks field_ranks(const ChannelStore& store, uint16_t channel_id,
                         size_t field_index);

std::vector<ChannelRanks> channel_ranks(const ChannelStore& store);

std::string describe_ranks(const ChannelRanks& r);

}

#endif
