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

// The timeline subsystem merges the events that the domain subsystems recover
// into one chronologically ordered view of a recording. The session
// reconstructor, the anomaly engine, and the channel store each produce their
// own event lists keyed by frame position; on their own they answer "what
// sessions were there" or "what faults fired", but a triage engineer wants the
// single ordered story of what the device did. This subsystem builds that story:
// it collects session boundaries, checkpoints, reconnects, markers, state
// transitions, config changes, faults, and sample-block arrivals into a flat
// list of typed events sorted by (device-monotonic time, session sequence), and
// offers the filtering and windowing a caller needs to narrow it.
//
// It reads only the reconstructed model; it never touches raw bytes and mutates
// nothing. Every routine is a pure function of its inputs. Nothing here throws.

namespace blackspan {

// The kind of a timeline event. The values double as bit positions for a kind
// mask (1u << kind), so a caller can select a subset of kinds in one filter.
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

// Number of distinct timeline kinds; the mask uses the low kMaxTimelineKind bits.
constexpr size_t kTimelineKindCount = 9;

// Lowercase, hyphenated name of a kind ("session-begin", "state-transition",
// ...). Never null.
const char* timeline_kind_name(TimelineKind k);

// A bit for a kind, suitable for building a kind mask.
constexpr uint32_t timeline_kind_bit(TimelineKind k) {
  return 1u << static_cast<unsigned>(k);
}

// A kind mask selecting every kind.
constexpr uint32_t kTimelineAllKinds = (1u << kTimelineKindCount) - 1u;

// One event on the merged timeline. `detail` is a short human-readable summary
// specific to the kind (e.g. a fault code and severity, a transition's from/to
// states, a sample block's channel and count). `channel_id` and `count` are
// meaningful only for SampleBlock events and are 0 otherwise; `severity` is
// meaningful only for Fault events.
struct TimelineEvent {
  uint64_t monotonic_ts = 0;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  TimelineKind kind = TimelineKind::Marker;
  uint16_t channel_id = 0;   // SampleBlock only
  uint32_t count = 0;        // SampleBlock sample count only
  Severity severity = Severity::Info;  // Fault only
  std::string detail;
};

// A one-line rendering of an event: its timestamp, device, kind, and detail.
std::string describe_timeline_event(const TimelineEvent& e);

// Options controlling how a timeline is built and which events it retains.
struct TimelineOptions {
  // When set, only events for this device are kept.
  std::optional<uint32_t> device;
  // A mask of (1u << kind) bits; only selected kinds are kept. Defaults to all.
  uint32_t kind_mask = kTimelineAllKinds;
  // Inclusive time bounds; an unset bound is unbounded on that side.
  std::optional<uint64_t> since;
  std::optional<uint64_t> until;
};

// The merged, ordered timeline of a recording, plus the counts needed to
// summarize it without walking the events again.
class Timeline {
 public:
  // Builds a timeline from a decoded recording under the given options.
  static Timeline build(const DecodedRecording& rec,
                        const TimelineOptions& opts = {});

  // Builds a timeline directly from the component subsystems, for callers that
  // reconstructed them separately. `channels` may be null to omit SampleBlock
  // events.
  static Timeline build(const std::vector<Session>& sessions,
                        const AnomalyEngine& anomalies,
                        const ChannelStore* channels,
                        const TimelineOptions& opts = {});

  const std::vector<TimelineEvent>& events() const { return events_; }
  size_t size() const { return events_.size(); }
  bool empty() const { return events_.empty(); }

  // Count of retained events of a given kind.
  size_t count_of(TimelineKind k) const;

  // The distinct device ids present, ascending.
  std::vector<uint32_t> devices() const;

  // The timestamp span [first, last] of the retained events, or nullopt when the
  // timeline is empty.
  std::optional<std::pair<uint64_t, uint64_t>> span() const;

  // Events falling in the inclusive time window [lo, hi], as pointers into this
  // timeline's storage (valid until the timeline is destroyed). The result is in
  // timeline order.
  std::vector<const TimelineEvent*> in_window(uint64_t lo, uint64_t hi) const;

  // Splits the timeline span into buckets of width `window_ns` and returns the
  // event count in each, earliest bucket first. The first bucket starts at the
  // first event's timestamp. A window_ns of 0, or an empty timeline, yields an
  // empty vector.
  std::vector<size_t> density(uint64_t window_ns) const;

 private:
  std::vector<TimelineEvent> events_;
};

// Renders a timeline to a deterministic multi-line text block: one line per
// event, preceded by a header line with the event count and span. An empty
// timeline renders a single "0 events" line.
std::string render_timeline(const Timeline& tl);

}  // namespace blackspan

#endif  // BLACKSPAN_TIMELINE_H
