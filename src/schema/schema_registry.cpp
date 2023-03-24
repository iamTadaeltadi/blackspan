#include "blackspan/schema.h"

namespace blackspan {

const ChannelSchema* SchemaRegistry::register_schema(const ChannelSchema& s,
                                                     SchemaError* err) {
  ChannelSchema canon = s;
  canon.recompute_layout();
  const SchemaError v = canon.validate();
  if (v != SchemaError::None) {
    if (err) *err = v;
    return nullptr;
  }
  if (err) *err = SchemaError::None;

  std::deque<ChannelSchema>& history = channels_[canon.channel_id];
  if (!history.empty()) {
    const ChannelSchema& prev = history.back();
    if (prev.same_layout(canon)) {
      return &prev;
    }
    canon.version = static_cast<uint16_t>(prev.version + 1);
  } else {
    canon.version = 1;
  }
  history.push_back(std::move(canon));
  return &history.back();
}

const ChannelSchema* SchemaRegistry::latest(uint16_t channel_id) const {
  const auto it = channels_.find(channel_id);
  if (it == channels_.end() || it->second.empty()) return nullptr;
  return &it->second.back();
}

const ChannelSchema* SchemaRegistry::get(uint16_t channel_id,
                                         uint16_t version) const {
  const auto it = channels_.find(channel_id);
  if (it == channels_.end()) return nullptr;
  for (const ChannelSchema& cs : it->second) {
    if (cs.version == version) return &cs;
  }
  return nullptr;
}

bool SchemaRegistry::contains(uint16_t channel_id) const {
  const auto it = channels_.find(channel_id);
  return it != channels_.end() && !it->second.empty();
}

size_t SchemaRegistry::count() const { return channels_.size(); }

size_t SchemaRegistry::total_versions() const {
  size_t total = 0;
  for (const auto& kv : channels_) total += kv.second.size();
  return total;
}

std::vector<uint16_t> SchemaRegistry::versions(uint16_t channel_id) const {
  std::vector<uint16_t> out;
  const auto it = channels_.find(channel_id);
  if (it == channels_.end()) return out;
  out.reserve(it->second.size());
  for (const ChannelSchema& cs : it->second) out.push_back(cs.version);
  return out;
}

std::vector<uint16_t> SchemaRegistry::channels() const {
  std::vector<uint16_t> out;
  out.reserve(channels_.size());
  for (const auto& kv : channels_) out.push_back(kv.first);
  return out;
}

SchemaError SchemaRegistry::ingest_frame(const Frame& f) {
  if (f.header.type != RecordType::SchemaDef) {
    return SchemaError::NotSchemaFrame;
  }
  const SchemaDecodeResult r =
      decode_channel_schema(f.payload.data(), f.payload.size());
  if (!r.ok()) return r.error;
  SchemaError err = SchemaError::None;
  register_schema(r.schema, &err);
  return err;
}

}
