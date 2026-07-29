#ifndef BLACKSPAN_GAUGE_H
#define BLACKSPAN_GAUGE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

struct Gauge {
  uint16_t channel_id = 0;
  size_t index = 0;
  double level = 0.0;
};

class GaugeBoard {
 public:
  GaugeBoard();

  void watch(uint16_t channel_id, size_t index, double level);
  double aggregate() const;
  size_t size() const { return gauges_.size(); }

 private:
  std::vector<Gauge> gauges_;
  std::vector<std::function<double()>> readers_;
};

std::vector<double> channel_gauge_totals(const ChannelStore& store);

}

#endif
