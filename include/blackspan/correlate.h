#ifndef BLACKSPAN_CORRELATE_H
#define BLACKSPAN_CORRELATE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/detect.h"
#include "blackspan/pipeline.h"

namespace blackspan {

enum class EventClass : uint8_t { Detection, Fault, Transition };

const char* event_class_name(EventClass c);

struct CorrelatedEvent {
  EventClass cls = EventClass::Detection;
  uint32_t device_id = 0;
  uint64_t ts = 0;
  uint32_t code = 0;
};

struct Correlation {
  uint32_t device_id = 0;
  uint64_t anchor_ts = 0;
  uint64_t last_member_ts = 0;
  size_t detections = 0;
  size_t faults = 0;
  size_t transitions = 0;
  std::vector<CorrelatedEvent> members;
};

struct CorrelationConfig {
  uint64_t window_ns = 1000000;
  size_t min_members = 2;
};

std::vector<Correlation> correlate_events(const std::vector<CorrelatedEvent>& events,
                                          const CorrelationConfig& cfg);

std::vector<CorrelatedEvent> gather_events(const DecodedRecording& rec,
                                           const std::vector<DetectRule>& rules);

std::vector<Correlation> correlate_recording(const DecodedRecording& rec,
                                             const std::vector<DetectRule>& rules,
                                             const CorrelationConfig& cfg);

std::string describe_correlation(const Correlation& c);

uint64_t correlation_span(const Correlation& c);

bool correlation_contains(const Correlation& c, EventClass cls);

uint64_t correlation_strength(const Correlation& c);

std::vector<size_t> rank_correlations(const std::vector<Correlation>& cs);

std::vector<Correlation> correlations_for_device(
    const std::vector<Correlation>& cs, uint32_t device);

std::vector<Correlation> correlations_with_class(
    const std::vector<Correlation>& cs, EventClass cls);

std::string render_correlations(const std::vector<Correlation>& cs);

std::vector<Correlation> merge_correlations(const std::vector<Correlation>& cs,
                                            uint64_t span_ns);

struct CorrelationSummary {
  size_t total = 0;
  size_t detections = 0;
  size_t faults = 0;
  size_t transitions = 0;
  size_t device_count = 0;
  size_t largest_members = 0;
  uint64_t widest_span = 0;
};

CorrelationSummary summarize_correlations(const std::vector<Correlation>& cs);

std::string describe_correlation_summary(const CorrelationSummary& s);

struct Coincidence {
  uint32_t device_id = 0;
  uint64_t detection_ts = 0;
  uint32_t detection_code = 0;
  uint64_t fault_ts = 0;
  uint32_t fault_code = 0;
  uint64_t gap = 0;
};

std::vector<Coincidence> detection_fault_coincidences(
    const std::vector<CorrelatedEvent>& events, uint64_t window_ns);

std::string describe_coincidence(const Coincidence& c);

}

#endif
