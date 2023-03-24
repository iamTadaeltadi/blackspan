#ifndef BLACKSPAN_AGGREGATE_H
#define BLACKSPAN_AGGREGATE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

enum class AggFn : uint8_t {
  Mean = 0,
  Min = 1,
  Max = 2,
  First = 3,
  Last = 4,
  Count = 5,
  Sum = 6,
};

const char* agg_fn_name(AggFn f);

bool parse_agg_fn(const std::string& s, AggFn& out);

struct AggBucket {
  uint64_t window_start = 0;
  size_t count = 0;
  double value = 0.0;
};

std::vector<std::pair<uint64_t, double>> column_with_timestamps(
    const ChannelStore& store, uint16_t channel_id, size_t field_index);

std::vector<AggBucket> aggregate_channel(const ChannelStore& store,
                                         uint16_t channel_id,
                                         size_t field_index, uint64_t window_ns,
                                         AggFn fn);

std::vector<AggBucket> aggregate_series(
    const std::vector<std::pair<uint64_t, double>>& samples, uint64_t window_ns,
    AggFn fn);

std::string render_buckets(const std::vector<AggBucket>& buckets, AggFn fn);

}

#endif
