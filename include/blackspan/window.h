#ifndef BLACKSPAN_WINDOW_H
#define BLACKSPAN_WINDOW_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

struct SampleWindow {
  const double* data = nullptr;
  size_t len = 0;
};

class WindowCache {
 public:
  SampleWindow recent(const ChannelStore& store, uint16_t channel);

 private:
  std::vector<double> stable_;
};

double window_peak(const SampleWindow& window);

std::vector<double> channel_window_peaks(const ChannelStore& store);

}

#endif
