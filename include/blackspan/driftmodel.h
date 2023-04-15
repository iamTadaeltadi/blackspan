#ifndef BLACKSPAN_DRIFTMODEL_H
#define BLACKSPAN_DRIFTMODEL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/pipeline.h"

namespace blackspan {

std::vector<double> derived_residuals(const DecodedRecording& rec,
                                      const CachedDerived& d);

struct ChannelDrift {
  uint16_t derived_id = 0;
  uint16_t primary_channel = 0;
  size_t samples = 0;
  double residual_rms = 0.0;
  double max_abs = 0.0;
};

std::vector<ChannelDrift> channel_drift(const DecodedRecording& rec);

std::string describe_drift(const ChannelDrift& d);

}

#endif
