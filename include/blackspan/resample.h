#ifndef BLACKSPAN_RESAMPLE_H
#define BLACKSPAN_RESAMPLE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

enum class ResampleMethod : uint8_t {
  Nearest = 0,
  PreviousHold = 1,
  Linear = 2,
};

const char* resample_method_name(ResampleMethod m);

bool parse_resample_method(const std::string& s, ResampleMethod& out);

struct ResampleGrid {
  uint64_t start = 0;
  uint64_t step = 1;
  size_t count = 0;

  uint64_t at(size_t i) const { return start + static_cast<uint64_t>(i) * step; }
};

ResampleGrid grid_from_series(const std::vector<std::pair<uint64_t, double>>& in,
                              uint64_t step);

struct ResamplePlan {
  ResampleMethod method = ResampleMethod::Linear;
  ResampleGrid grid;

  uint64_t max_gap_ns = 0;
};

struct ResampledSeries {
  std::vector<uint64_t> ts;
  std::vector<double> values;
  size_t defined = 0;
  size_t nan = 0;
};

ResampledSeries resample_series(const std::vector<std::pair<uint64_t, double>>& in,
                                const ResamplePlan& plan);

ResampledSeries resample_channel(const ChannelStore& store, uint16_t channel_id,
                                 size_t field_index, const ResamplePlan& plan);

ResampledSeries resample_channel(const ChannelStore& store, uint16_t channel_id,
                                 const std::string& field_name,
                                 const ResamplePlan& plan);

}

#endif
