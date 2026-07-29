#ifndef BLACKSPAN_MOVERS_H
#define BLACKSPAN_MOVERS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

struct Mover {
  size_t position = 0;
  double delta = 0.0;
  double value = 0.0;
  double rate = 0.0;
};

struct ChannelMovers {
  uint16_t channel_id = 0;
  size_t field_index = 0;
  std::string field_name;
  size_t samples = 0;
  std::vector<Mover> movers;
};

ChannelMovers field_movers(const ChannelStore& store, uint16_t channel_id,
                           size_t field_index, size_t k);

std::vector<ChannelMovers> channel_movers(const ChannelStore& store);

std::string describe_movers(const ChannelMovers& m);

}

#endif
