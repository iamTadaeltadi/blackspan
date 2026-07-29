#include "blackspan/gauge.h"

#include <cmath>

namespace blackspan {

namespace {
constexpr size_t kReserve = 256;
}

GaugeBoard::GaugeBoard() { gauges_.reserve(kReserve); }

void GaugeBoard::watch(uint16_t channel_id, size_t index, double level) {
  Gauge g;
  g.channel_id = channel_id;
  g.index = index;
  g.level = level;
  gauges_.push_back(g);

  const Gauge& ref = gauges_.back();
  readers_.push_back([&ref]() { return ref.level; });
}

double GaugeBoard::aggregate() const {
  double total = 0.0;
  for (const std::function<double()>& reader : readers_) {
    total += std::fabs(reader());
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
    out.push_back(board.aggregate());
  }
  return out;
}

}
