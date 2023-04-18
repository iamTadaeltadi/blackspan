#ifndef BLACKSPAN_RETENTION_H
#define BLACKSPAN_RETENTION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/frame.h"

namespace blackspan {

enum class RetentionPriority : uint8_t {
  Faults = 0,
  StateTransitions = 1,
  Sessions = 2,
  Samples = 3,
  Markers = 4,
  ConfigChanges = 5,
  Other = 6,
};

constexpr size_t kRetentionPriorityCount = 7;

RetentionPriority priority_of(RecordType t);

const char* retention_priority_name(RetentionPriority p);

std::vector<RetentionPriority> default_priority_order();

struct RetentionPolicy {
  uint64_t keep_since_ts = 0;
  uint64_t keep_until_ts = UINT64_MAX;
  size_t max_frames = 0;
  size_t max_bytes = 0;
  size_t keep_tail = 0;
  bool drop_samples = false;
  bool drop_markers = false;
  bool keep_session_framing = false;
  std::vector<RetentionPriority> priority_order;

  bool keeps(const FrameHeader& h) const;
};

struct RetentionPlan {
  size_t frames_in = 0;
  size_t frames_kept = 0;
  size_t bytes_in = 0;
  size_t bytes_kept = 0;
  std::vector<size_t> kept_indices;
  std::array<size_t, kRetentionPriorityCount> kept_by_priority{};
  FrameStatus input_status = FrameStatus::Ok;
};

RetentionPlan plan_retention(const uint8_t* data, size_t len,
                             const RetentionPolicy& policy);

RetentionPlan plan_retention(const std::vector<uint8_t>& data,
                             const RetentionPolicy& policy);

std::vector<uint8_t> apply_retention(const uint8_t* data, size_t len,
                                     const RetentionPolicy& policy);

std::vector<uint8_t> apply_retention(const std::vector<uint8_t>& data,
                                     const RetentionPolicy& policy);

double retention_ratio(const RetentionPlan& plan);

size_t frames_dropped(const RetentionPlan& plan);

size_t bytes_dropped(const RetentionPlan& plan);

std::vector<size_t> dropped_indices(const RetentionPlan& plan);

std::string describe_plan(const RetentionPlan& plan);

std::string describe_policy(const RetentionPolicy& policy);

}

#endif
