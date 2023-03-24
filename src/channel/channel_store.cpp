#include <cstdint>

#include "blackspan/channel.h"

namespace blackspan {

ChannelError ChannelStore::ingest_frame(const Frame& f,
                                        const SchemaRegistry& registry) {
  if (f.header.type != RecordType::SampleBlock) {
    return ChannelError::NotSampleBlock;
  }

  SampleBlockHeader h;
  const ChannelError he =
      decode_sample_block_header(f.payload.data(), f.payload.size(), h);
  if (he != ChannelError::None) return he;

  const ChannelSchema* schema = registry.get(h.channel_id, h.schema_version);
  if (schema == nullptr) {
    return registry.contains(h.channel_id) ? ChannelError::UnknownVersion
                                           : ChannelError::UnknownChannel;
  }

  const auto existing = series_.find(h.channel_id);
  if (existing != series_.end() &&
      existing->second.schema_version() != h.schema_version) {
    return ChannelError::VersionMismatch;
  }

  const size_t stride = schema->stride;
  if (stride == 0) return ChannelError::ZeroStride;
  if (h.sample_count == 0) return ChannelError::ZeroCount;

  const size_t count = h.sample_count;
  if (count > SIZE_MAX / stride) return ChannelError::StrideOverflow;
  const size_t rows_bytes = count * stride;
  if (rows_bytes > SIZE_MAX - kSampleBlockHeaderSize) {
    return ChannelError::StrideOverflow;
  }
  const size_t expected = kSampleBlockHeaderSize + rows_bytes;
  if (f.payload.size() != expected) return ChannelError::LengthMismatch;

  ChannelSeries* target;
  if (existing != series_.end()) {
    target = &existing->second;
  } else {
    target = &series_.emplace(h.channel_id, ChannelSeries(schema)).first->second;
  }

  const uint64_t base_ts = f.header.monotonic_ts + h.base_ts_delta;
  const uint8_t* rows = f.payload.data() + kSampleBlockHeaderSize;
  target->append_block(rows, rows_bytes, h.sample_count, h.flags, base_ts);

  if (target->total_samples() > kChannelRetentionCap) {
    target->compact_to(kChannelRetentionCap);
  }
  return ChannelError::None;
}

const ChannelSeries* ChannelStore::series(uint16_t channel_id) const {
  const auto it = series_.find(channel_id);
  if (it == series_.end()) return nullptr;
  return &it->second;
}

std::vector<uint16_t> ChannelStore::channels() const {
  std::vector<uint16_t> out;
  out.reserve(series_.size());
  for (const auto& kv : series_) out.push_back(kv.first);
  return out;
}

size_t ChannelStore::total_samples() const {
  size_t total = 0;
  for (const auto& kv : series_) total += kv.second.total_samples();
  return total;
}

ChannelError ChannelStore::compact_channel(uint16_t channel_id,
                                           size_t keep_last_n) {
  const auto it = series_.find(channel_id);
  if (it == series_.end()) return ChannelError::UnknownChannel;
  it->second.compact_to(keep_last_n);
  return ChannelError::None;
}

size_t ChannelStore::retained_samples(uint16_t channel_id) const {
  const ChannelSeries* s = series(channel_id);
  return s ? s->total_samples() : 0;
}

std::vector<double> ChannelStore::column_as_doubles(uint16_t channel_id,
                                                    size_t field_index) const {
  std::vector<double> out;
  const ChannelSeries* s = series(channel_id);
  if (s == nullptr) return out;
  const size_t n = s->total_samples();
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const std::optional<SampleView> v = s->view(i);
    if (!v) continue;
    const std::optional<double> d = v->get_as_double(field_index);
    if (d) out.push_back(*d);
  }
  return out;
}

}
