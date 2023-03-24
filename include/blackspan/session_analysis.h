#ifndef BLACKSPAN_SESSION_ANALYSIS_H
#define BLACKSPAN_SESSION_ANALYSIS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/session.h"

namespace blackspan {

struct DeviceActivity {
  uint32_t device_id = 0;
  size_t session_count = 0;
  uint64_t first_begin_ts = 0;
  uint64_t last_end_ts = 0;
  uint64_t total_active_ns = 0;
  uint64_t total_gap_ns = 0;
  double duty_cycle = 0.0;
  size_t total_checkpoints = 0;
  size_t total_reconnects = 0;
  size_t total_markers = 0;
  size_t unclosed_sessions = 0;
  size_t overlaps = 0;
  size_t seq_regressions = 0;
  size_t ts_regressions = 0;
};

struct SessionGap {
  uint32_t device_id = 0;
  uint32_t after_index = 0;
  uint32_t before_index = 0;
  uint64_t gap_ns = 0;
};

std::vector<DeviceActivity> analyze_activity(const std::vector<Session>& sessions);

std::vector<SessionGap> session_gaps(const std::vector<Session>& sessions);

double device_duty_cycle(const std::vector<Session>& sessions,
                         uint32_t device_id);

std::string describe_activity(const DeviceActivity& a);

struct FleetActivity {
  size_t device_count = 0;
  size_t session_count = 0;
  uint64_t earliest_begin_ts = 0;
  uint64_t latest_end_ts = 0;
  uint64_t total_active_ns = 0;
  uint64_t total_gap_ns = 0;
  double aggregate_duty_cycle = 0.0;
  double mean_duty_cycle = 0.0;
  size_t total_checkpoints = 0;
  size_t total_reconnects = 0;
  size_t total_markers = 0;
  size_t unclosed_sessions = 0;
  size_t total_overlaps = 0;
  uint32_t busiest_device = 0;
  uint64_t busiest_active_ns = 0;
  bool has_devices = false;
};

FleetActivity analyze_fleet(const std::vector<DeviceActivity>& per_device);

std::string describe_fleet(const FleetActivity& f);

bool longest_idle_gap(const std::vector<Session>& sessions, SessionGap& out);

}

#endif
