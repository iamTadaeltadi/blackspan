#include "blackspan/profile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace blackspan {

namespace {
constexpr size_t kActivityLevels = 512;
}

ActivityProfile field_activity(const ChannelStore& store, uint16_t channel_id,
                               size_t field_index) {
  ActivityProfile p;
  p.channel_id = channel_id;
  p.field_index = field_index;

  const ChannelSeries* series = store.series(channel_id);
  if (series == nullptr) return p;
  if (field_index < series->schema().fields.size()) {
    p.field_name = series->schema().fields[field_index].name;
  }

  const std::vector<double> col = store.column_as_doubles(channel_id, field_index);
  const size_t n = col.size();
  p.samples = n;
  if (n == 0) return p;

  double sum = 0.0;
  for (double v : col) sum += v;
  p.mean = sum / static_cast<double>(n);

  const size_t width = std::min(kActivityLevels, n);
  p.level.assign(width, 0.0f);
  for (size_t i = 0; i < width; ++i) {
    p.level[i] = static_cast<float>(col[i] - p.mean);
  }
  return p;
}

std::vector<ActivityProfile> channel_activity(const ChannelStore& store) {
  std::vector<ActivityProfile> out;
  for (uint16_t id : store.channels()) {
    const ChannelSeries* series = store.series(id);
    if (series == nullptr) continue;
    const size_t fields = series->schema().fields.size();
    for (size_t f = 0; f < fields; ++f) {
      out.push_back(field_activity(store, id, f));
    }
  }
  return out;
}

std::string describe_activity(const ActivityProfile& p) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6g", p.mean);
  return "channel " + std::to_string(p.channel_id) + " field " + p.field_name +
         " mean " + buf + " over " + std::to_string(p.samples) + " samples";
}

}
