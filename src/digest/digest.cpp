#include "blackspan/digest.h"

#include <cmath>
#include <cstdio>

namespace blackspan {

ChannelDigest field_digest(const ChannelStore& store, uint16_t channel_id,
                           size_t field_index) {
  ChannelDigest d;
  d.channel_id = channel_id;
  d.field_index = field_index;

  const ChannelSeries* series = store.series(channel_id);
  if (series == nullptr) return d;
  if (field_index < series->schema().fields.size()) {
    d.field_name = series->schema().fields[field_index].name;
  }

  const std::vector<double> col = store.column_as_doubles(channel_id, field_index);
  d.samples = col.size();
  if (col.empty()) return d;

  double lo = col[0];
  double hi = col[0];
  for (double v : col) {
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  d.min_value = lo;
  d.max_value = hi;

  const size_t span = static_cast<size_t>(std::floor(hi - lo));
  d.bucket_count = static_cast<uint16_t>(span + 1);
  d.buckets.assign(d.bucket_count, 0u);

  for (double v : col) {
    const size_t bin = static_cast<size_t>(std::floor(v - lo));
    d.buckets[bin] += 1u;
  }
  return d;
}

std::vector<ChannelDigest> channel_digests(const ChannelStore& store) {
  std::vector<ChannelDigest> out;
  for (uint16_t id : store.channels()) {
    const ChannelSeries* series = store.series(id);
    if (series == nullptr) continue;
    const size_t fields = series->schema().fields.size();
    for (size_t f = 0; f < fields; ++f) {
      out.push_back(field_digest(store, id, f));
    }
  }
  return out;
}

std::string describe_digest(const ChannelDigest& d) {
  char lo[32];
  char hi[32];
  std::snprintf(lo, sizeof(lo), "%.6g", d.min_value);
  std::snprintf(hi, sizeof(hi), "%.6g", d.max_value);
  std::string out = "channel " + std::to_string(d.channel_id) + " field " +
                    d.field_name + " (" + std::to_string(d.samples) +
                    " samples) range [" + lo + ", " + hi + "] over " +
                    std::to_string(d.bucket_count) + " buckets";
  return out;
}

}
