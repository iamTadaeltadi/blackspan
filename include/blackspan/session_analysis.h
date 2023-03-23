#ifndef BLACKSPAN_SESSION_ANALYSIS_H
#define BLACKSPAN_SESSION_ANALYSIS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/session.h"

// The session-analysis subsystem is a higher-level view over the reconstructed
// session list: the per-device operational summary that triage reaches for
// first. Where the session reconstructor answers "what were the sessions?", this
// answers "how did each device behave across all of its sessions?" — how long it
// was recording versus idle, how often it checkpointed or reconnected, and
// whether any sessions were left open.
//
// Everything here is pure and read-only. It borrows the session list produced by
// the reconstructor (DecodedRecording::sessions().sessions()), computes rollups,
// and returns them by value. It mutates nothing and never throws: degenerate
// input (no sessions, a single session, zero-duration sessions, overlapping or
// out-of-order sessions) yields well-defined, documented results rather than an
// error.
//
// Timeline model. A device's sessions are grouped together and ordered by
// begin_ts (ties broken by session_index, so the ordering is total and stable).
// "Active" time is the sum of the sessions' own durations; a session's duration
// is Session::duration_ns(), which is the closed span end_ts - begin_ts and is 0
// for any session that never closed cleanly. "Idle" time is the sum of the gaps
// between one session's end and the next session's begin. When the next session
// begins before the previous one ended (an overlap, which a well-formed recording
// never produces) the gap is clamped to 0 and the overlap is counted separately.
// For a clean, non-overlapping recording active + idle telescopes exactly to the
// device's wall span (last_end_ts - first_begin_ts).
//
// On top of the per-device rollup the subsystem offers a fleet-wide rollup
// (analyze_fleet) that folds the per-device results into one recording-level
// summary, and a longest-idle-gap query that surfaces the single darkest stretch
// in the recording. Both compose on the per-device layer and share its model.

namespace blackspan {

// Per-device rollup across all of a device's sessions.
struct DeviceActivity {
  uint32_t device_id = 0;
  size_t session_count = 0;
  uint64_t first_begin_ts = 0;   // begin_ts of the earliest session
  uint64_t last_end_ts = 0;      // end_ts of the latest session (by begin order)
  uint64_t total_active_ns = 0;  // sum of session durations
  uint64_t total_gap_ns = 0;     // sum of inter-session idle gaps
  double duty_cycle = 0.0;       // active / (active + idle); 0 when that span is 0
  size_t total_checkpoints = 0;
  size_t total_reconnects = 0;
  size_t total_markers = 0;
  size_t unclosed_sessions = 0;  // sessions that never cleanly closed
  size_t overlaps = 0;           // consecutive pairs whose spans overlapped
  size_t seq_regressions = 0;    // summed Session::seq_regressions
  size_t ts_regressions = 0;     // summed Session::ts_regressions
};

// One idle gap between two consecutive sessions of a device. `after_index` is the
// session_index the gap follows and `before_index` the one it precedes; both are
// Session::session_index values. An overlap is reported with gap_ns == 0.
struct SessionGap {
  uint32_t device_id = 0;
  uint32_t after_index = 0;
  uint32_t before_index = 0;
  uint64_t gap_ns = 0;
};

// Computes per-device activity across `sessions`, one DeviceActivity per distinct
// device_id, returned sorted by device_id ascending. Within each device the
// sessions are grouped and ordered by begin_ts (ties by session_index) before
// the rollup, so out-of-order input is handled defensively.
std::vector<DeviceActivity> analyze_activity(const std::vector<Session>& sessions);

// Every inter-session idle gap, per device. Gaps are emitted device_id ascending
// and, within a device, in begin_ts order. A gap whose next session begins before
// the previous one ended (an overlap) is clamped to 0 rather than dropped, so the
// count of gaps for a device with n sessions is always n - 1 (0 for n < 2).
std::vector<SessionGap> session_gaps(const std::vector<Session>& sessions);

// Duty cycle (active / wall span) for one device: total active time divided by
// active + idle time. Returns 0.0 when the device has no sessions or when that
// span is 0 (e.g. a single zero-duration session). This is exactly the
// duty_cycle that analyze_activity reports for the same device.
double device_duty_cycle(const std::vector<Session>& sessions,
                         uint32_t device_id);

// A one-line human summary of a device's activity, in the domain describe_* style.
// Always non-empty, including for a default-constructed DeviceActivity.
std::string describe_activity(const DeviceActivity& a);

// Recording-level rollup folded from every device's activity: the totals triage
// reads before drilling into a single device.
struct FleetActivity {
  size_t device_count = 0;
  size_t session_count = 0;
  uint64_t earliest_begin_ts = 0;      // min first_begin_ts across devices
  uint64_t latest_end_ts = 0;          // max last_end_ts across devices
  uint64_t total_active_ns = 0;        // active time summed over all devices
  uint64_t total_gap_ns = 0;           // idle time summed over all devices
  double aggregate_duty_cycle = 0.0;   // total_active / (total_active + total_gap)
  double mean_duty_cycle = 0.0;        // unweighted mean of per-device duty cycles
  size_t total_checkpoints = 0;
  size_t total_reconnects = 0;
  size_t total_markers = 0;
  size_t unclosed_sessions = 0;
  size_t total_overlaps = 0;
  uint32_t busiest_device = 0;         // device with the greatest active time
  uint64_t busiest_active_ns = 0;      // its active time
  bool has_devices = false;            // false only for an empty input
};

// Folds per-device rollups (as produced by analyze_activity) into one
// fleet-wide summary. `aggregate_duty_cycle` weights every nanosecond equally,
// while `mean_duty_cycle` weights every device equally; the two diverge when a
// short-lived and a long-lived device sit in the same recording. On a tie for
// busiest device the lower device_id wins. An empty input yields a zeroed
// FleetActivity with has_devices == false.
FleetActivity analyze_fleet(const std::vector<DeviceActivity>& per_device);

// A one-line human summary of a fleet rollup, in the domain describe_* style.
// Always non-empty, including for an empty fleet.
std::string describe_fleet(const FleetActivity& f);

// Finds the single longest inter-session idle gap in the whole recording and
// writes it to *out. Returns false (leaving *out untouched) when the recording
// has no inter-session gaps at all — fewer than two sessions on every device. On
// a tie the earlier gap wins (device_id ascending, then begin order), matching
// session_gaps() ordering. An all-overlap recording reports a 0-ns gap.
bool longest_idle_gap(const std::vector<Session>& sessions, SessionGap& out);

}  // namespace blackspan

#endif  // BLACKSPAN_SESSION_ANALYSIS_H
