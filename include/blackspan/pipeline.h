#ifndef BLACKSPAN_PIPELINE_H
#define BLACKSPAN_PIPELINE_H

#include <cstddef>
#include <cstdint>

#include "blackspan/anomaly.h"
#include "blackspan/channel.h"
#include "blackspan/frame.h"
#include "blackspan/schema.h"
#include "blackspan/session.h"

namespace blackspan {

// Decodes a recording exactly once, routing every frame to each subsystem, and
// holds the resulting reconstructed model: the schema registry, the session
// reconstruction, the channel column stores, and the anomaly engine.
//
// This type is non-movable on purpose. The channel store keeps borrowed
// pointers to schemas owned by the registry held here, so the whole object must
// stay in place for as long as any part of the model is read.
class DecodedRecording {
 public:
  DecodedRecording() = default;
  ~DecodedRecording() = default;
  DecodedRecording(const DecodedRecording&) = delete;
  DecodedRecording& operator=(const DecodedRecording&) = delete;
  DecodedRecording(DecodedRecording&&) = delete;
  DecodedRecording& operator=(DecodedRecording&&) = delete;

  // Runs one FrameStream over the buffer, routing each frame: schema-def frames
  // register schemas, sample-block frames append to channel series (resolved
  // against the schemas seen so far), and every frame is offered to the session
  // reconstructor and anomaly engine, which act on the record types they own.
  // Intended to be called once on a fresh instance.
  void decode(const uint8_t* data, size_t len);

  const SchemaRegistry& schemas() const { return schemas_; }
  const SessionReconstructor& sessions() const { return sessions_; }
  const ChannelStore& channels() const { return channels_; }
  const AnomalyEngine& anomalies() const { return anomalies_; }

  // Number of complete frames decoded.
  size_t frame_count() const { return frame_count_; }

  // Final frame-stream status: Ok when the buffer was fully consumed, Incomplete
  // for a trailing partial frame, or the status that halted decoding.
  FrameStatus final_status() const { return status_; }

 private:
  SchemaRegistry schemas_;
  SessionReconstructor sessions_;
  ChannelStore channels_;
  AnomalyEngine anomalies_;
  size_t frame_count_ = 0;
  FrameStatus status_ = FrameStatus::Ok;
};

}  // namespace blackspan

#endif  // BLACKSPAN_PIPELINE_H
