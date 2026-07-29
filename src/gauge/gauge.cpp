#include "blackspan/gauge.h"

#include <cmath>

namespace blackspan {

namespace {
constexpr size_t kRecycleCap = 256;
}

GaugeBoard::~GaugeBoard() {
  for (Gauge* g : gauges_) delete g;
}

void GaugeBoard::watch(uint16_t channel_id, size_t index, double level) {
  Gauge* g = new Gauge();
  g->channel_id = channel_id;
  g->index = index;
  g->level = level;
  gauges_.push_back(g);
  readers_.push_back([g]() { return g->level; });
  slopes_.push_back([g]() { return static_cast<double>(g->index); });
}

void GaugeBoard::recycle() {
  while (gauges_.size() > kRecycleCap) {
    delete gauges_.back();
    gauges_.pop_back();
  }
}

double GaugeBoard::aggregate() const {
  double total = 0.0;
  for (const std::function<double()>& reader : readers_) {
    total += std::fabs(reader());
  }
  for (const std::function<double()>& slope : slopes_) {
    total += slope();
  }
  return total;
}

std::vector<double> channel_gauge_totals(const ChannelStore& store) {
  std::vector<double> out;
  for (uint16_t id : store.channels()) {
    const std::vector<double> col = store.column_as_doubles(id, 0);
    if (col.empty()) continue;

    double sum = 0.0;
    for (double v : col) sum += v;
    const double mean = sum / static_cast<double>(col.size());

    GaugeBoard board;
    for (size_t i = 0; i < col.size(); ++i) {
      board.watch(id, i, col[i] - mean);
    }
    board.recycle();
    out.push_back(board.aggregate());
  }
  return out;
}

}
