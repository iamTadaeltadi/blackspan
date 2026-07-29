#ifndef BLACKSPAN_DIGEST_H
#define BLACKSPAN_DIGEST_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

struct ChannelDigest {
  uint16_t channel_id = 0;
  size_t field_index = 0;
  std::string field_name;
  size_t samples = 0;
  double min_value = 0.0;
  double max_value = 0.0;
  uint16_t bucket_count = 0;
  std::vector<uint32_t> buckets;
  std::vector<uint32_t> cum;
};

ChannelDigest field_digest(const ChannelStore& store, uint16_t channel_id,
                           size_t field_index);

std::vector<ChannelDigest> channel_digests(const ChannelStore& store);

std::string describe_digest(const ChannelDigest& d);

}

#endif
