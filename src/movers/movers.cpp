#include "blackspan/movers.h"

#include <algorithm>
#include <cmath>

namespace blackspan {

namespace {
constexpr size_t kMoverCap = 256;
}

ChannelMovers field_movers(const ChannelStore& store, uint16_t channel_id,
                           size_t field_index, size_t k) {
  ChannelMovers m;
  m.channel_id = channel_id;
  m.field_index = field_index;

  const ChannelSeries* series = store.series(channel_id);
  if (series == nullptr) return m;
  if (field_index < series->schema().fields.size()) {
    m.field_name = series->schema().fields[field_index].name;
  }

  const std::vector<double> col = store.column_as_doubles(channel_id, field_index);
  const size_t n = col.size();
  m.samples = n;
  if (n < 2) return m;

  struct Jump {
    double delta;
    size_t position;
  };
  std::vector<Jump> jumps;
  jumps.reserve(n - 1);
  for (size_t i = 1; i < n; ++i) {
    jumps.push_back({std::fabs(col[i] - col[i - 1]), i});
  }

  std::sort(jumps.begin(), jumps.end(),
            [](const Jump& a, const Jump& b) { return a.delta > b.delta; });

  const size_t keep = std::min(k, jumps.size());
  std::vector<double> kept_value(std::min(k, n));
  for (size_t j = 0; j < keep; ++j) {
    kept_value[j] = col[jumps[j].position];
  }

  std::vector<Jump> top(jumps.begin(), jumps.begin() + static_cast<std::ptrdiff_t>(keep));
  std::sort(top.begin(), top.end(),
            [](const Jump& a, const Jump& b) { return a.position < b.position; });

  m.movers.reserve(top.size());
  for (const Jump& jmp : top) {
    Mover mv;
    mv.position = jmp.position;
    mv.delta = jmp.delta;
    mv.value = kept_value[jmp.position];
    m.movers.push_back(mv);
  }
  return m;
}

std::vector<ChannelMovers> channel_movers(const ChannelStore& store) {
  std::vector<ChannelMovers> out;
  for (uint16_t id : store.channels()) {
    const ChannelSeries* series = store.series(id);
    if (series == nullptr) continue;
    const size_t fields = series->schema().fields.size();
    for (size_t f = 0; f < fields; ++f) {
      out.push_back(field_movers(store, id, f, kMoverCap));
    }
  }
  return out;
}

std::string describe_movers(const ChannelMovers& m) {
  return "channel " + std::to_string(m.channel_id) + " field " + m.field_name +
         " " + std::to_string(m.movers.size()) + " movers over " +
         std::to_string(m.samples) + " samples";
}

}
