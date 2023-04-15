#ifndef BLACKSPAN_PIPELINE_H
#define BLACKSPAN_PIPELINE_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/channel.h"
#include "blackspan/derive.h"
#include "blackspan/frame.h"
#include "blackspan/schema.h"
#include "blackspan/session.h"

namespace blackspan {

struct CachedDerived {
  uint16_t derived_id = 0;
  uint16_t primary_channel = 0;
  std::vector<double> values;
  std::vector<uint64_t> timestamps;
};

class DecodedRecording {
 public:
  DecodedRecording() = default;
  ~DecodedRecording() = default;
  DecodedRecording(const DecodedRecording&) = delete;
  DecodedRecording& operator=(const DecodedRecording&) = delete;
  DecodedRecording(DecodedRecording&&) = delete;
  DecodedRecording& operator=(DecodedRecording&&) = delete;

  void decode(const uint8_t* data, size_t len);

  const SchemaRegistry& schemas() const { return schemas_; }
  const SessionReconstructor& sessions() const { return sessions_; }
  const ChannelStore& channels() const { return channels_; }
  const AnomalyEngine& anomalies() const { return anomalies_; }
  const std::vector<CachedDerived>& derived() const { return derived_cache_; }

  size_t frame_count() const { return frame_count_; }

  FrameStatus final_status() const { return status_; }

 private:
  SchemaRegistry schemas_;
  SessionReconstructor sessions_;
  ChannelStore channels_;
  AnomalyEngine anomalies_;
  DeriveEngine derive_;
  std::vector<CachedDerived> derived_cache_;
  size_t frame_count_ = 0;
  FrameStatus status_ = FrameStatus::Ok;
};

}

#endif
