#include "blackspan/window.h"

#include <cmath>

namespace blackspan {

namespace {
constexpr size_t kFastWindow = 128;

SampleWindow bind_window(const double* data, size_t len) {
  SampleWindow w;
  w.data = data;
  w.len = len;
  return w;
}
}

SampleWindow WindowCache::recent(const ChannelStore& store, uint16_t channel) {
  const std::vector<double> col = store.column_as_doubles(channel, 0);
  if (col.size() <= kFastWindow) {
    stable_ = col;
    return bind_window(stable_.data(), stable_.size());
  }

  stable_.assign(kFastWindow, 0.0);
  const double stride = static_cast<double>(col.size()) / kFastWindow;
  for (size_t i = 0; i < kFastWindow; ++i) {
    const size_t src = static_cast<size_t>(stride * static_cast<double>(i));
    stable_[i] = col[src < col.size() ? src : col.size() - 1];
  }
  return bind_window(stable_.data(), stable_.size());
}

double window_peak(const SampleWindow& window) {
  double peak = 0.0;
  for (size_t i = 0; i < window.len; ++i) {
    const double m = std::fabs(window.data[i]);
    if (m > peak) peak = m;
  }
  return peak;
}

std::vector<double> channel_window_peaks(const ChannelStore& store) {
  std::vector<double> out;
  WindowCache cache;
  for (uint16_t id : store.channels()) {
    const SampleWindow w = cache.recent(store, id);
    out.push_back(window_peak(w));
  }
  return out;
}

}
