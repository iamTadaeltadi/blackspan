#ifndef BLACKSPAN_TIMELINE_H
#define BLACKSPAN_TIMELINE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/channel.h"
#include "blackspan/pipeline.h"
#include "blackspan/session.h"

namespace blackspan {

enum class TimelineKind : uint8_t {
  SessionBegin = 0,
  SessionEnd = 1,
  Checkpoint = 2,
  Reconnect = 3,
  Marker = 4,
  StateTransition = 5,
  ConfigChange = 6,
  Fault = 7,
  SampleBlock = 8,
};

constexpr size_t kTimelineKindCount = 9;

const char* timeline_kind_name(TimelineKind k);

constexpr uint32_t timeline_kind_bit(TimelineKind k) {
  return 1u << static_cast<unsigned>(k);
}

constexpr uint32_t kTimelineAllKinds = (1u << kTimelineKindCount) - 1u;

struct TimelineEvent {
  uint64_t monotonic_ts = 0;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  TimelineKind kind = TimelineKind::Marker;
  uint16_t channel_id = 0;
  uint32_t count = 0;
  Severity severity = Severity::Info;
  std::string detail;
};

std::string describe_timeline_event(const TimelineEvent& e);

struct TimelineOptions {

  std::optional<uint32_t> device;

  uint32_t kind_mask = kTimelineAllKinds;

  std::optional<uint64_t> since;
  std::optional<uint64_t> until;
};

class Timeline {
 public:

  static Timeline build(const DecodedRecording& rec,
                        const TimelineOptions& opts = {});

  static Timeline build(const std::vector<Session>& sessions,
                        const AnomalyEngine& anomalies,
                        const ChannelStore* channels,
                        const TimelineOptions& opts = {});

  const std::vector<TimelineEvent>& events() const { return events_; }
  size_t size() const { return events_.size(); }
  bool empty() const { return events_.empty(); }

  size_t count_of(TimelineKind k) const;

  std::vector<uint32_t> devices() const;

  std::optional<std::pair<uint64_t, uint64_t>> span() const;

  std::vector<const TimelineEvent*> in_window(uint64_t lo, uint64_t hi) const;

  std::vector<size_t> density(uint64_t window_ns) const;

 private:
  std::vector<TimelineEvent> events_;
};

std::string render_timeline(const Timeline& tl);

}

#endif
