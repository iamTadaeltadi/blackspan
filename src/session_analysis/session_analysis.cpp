#include "blackspan/session_analysis.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace blackspan {

namespace {

bool earlier(const Session* a, const Session* b) {
  if (a->begin_ts != b->begin_ts) return a->begin_ts < b->begin_ts;
  return a->session_index < b->session_index;
}

std::map<uint32_t, std::vector<const Session*>> group_sessions(
    const std::vector<Session>& sessions) {
  std::map<uint32_t, std::vector<const Session*>> by_device;
  for (const Session& s : sessions) by_device[s.device_id].push_back(&s);
  for (auto& kv : by_device) {
    std::sort(kv.second.begin(), kv.second.end(), earlier);
  }
  return by_device;
}

uint64_t gap_between(uint64_t prev_end, uint64_t next_begin, bool* overlap) {
  if (next_begin >= prev_end) {
    *overlap = false;
    return next_begin - prev_end;
  }
  *overlap = true;
  return 0;
}

DeviceActivity fold_device(uint32_t device_id,
                           const std::vector<const Session*>& ordered) {
  DeviceActivity a;
  a.device_id = device_id;
  a.session_count = ordered.size();
  if (ordered.empty()) return a;

  a.first_begin_ts = ordered.front()->begin_ts;
  a.last_end_ts = ordered.back()->end_ts;

  for (size_t i = 0; i < ordered.size(); ++i) {
    const Session* s = ordered[i];
    a.total_active_ns += s->duration_ns();
    a.total_checkpoints += s->checkpoints.size();
    a.total_reconnects += s->reconnects.size();
    a.total_markers += s->markers.size();
    if (!s->closed) ++a.unclosed_sessions;
    a.seq_regressions += s->seq_regressions;
    a.ts_regressions += s->ts_regressions;

    if (i > 0) {
      bool overlap = false;
      a.total_gap_ns +=
          gap_between(ordered[i - 1]->end_ts, s->begin_ts, &overlap);
      if (overlap) ++a.overlaps;
    }
  }

  const uint64_t span = a.total_active_ns + a.total_gap_ns;
  if (span > 0) {
    a.duty_cycle = static_cast<double>(a.total_active_ns) /
                   static_cast<double>(span);
  }
  return a;
}

std::string hex8(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08x", v);
  return b;
}

std::string duty_str(double d) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.2f", d);
  return b;
}

}

std::vector<DeviceActivity> analyze_activity(
    const std::vector<Session>& sessions) {
  std::vector<DeviceActivity> out;
  const auto by_device = group_sessions(sessions);
  out.reserve(by_device.size());
  for (const auto& kv : by_device) {
    out.push_back(fold_device(kv.first, kv.second));
  }
  return out;
}

std::vector<SessionGap> session_gaps(const std::vector<Session>& sessions) {
  std::vector<SessionGap> out;
  const auto by_device = group_sessions(sessions);
  for (const auto& kv : by_device) {
    const std::vector<const Session*>& ordered = kv.second;
    for (size_t i = 1; i < ordered.size(); ++i) {
      bool overlap = false;
      SessionGap g;
      g.device_id = kv.first;
      g.after_index = ordered[i - 1]->session_index;
      g.before_index = ordered[i]->session_index;
      g.gap_ns =
          gap_between(ordered[i - 1]->end_ts, ordered[i]->begin_ts, &overlap);
      out.push_back(g);
    }
  }
  return out;
}

double device_duty_cycle(const std::vector<Session>& sessions,
                         uint32_t device_id) {
  std::vector<const Session*> ordered;
  for (const Session& s : sessions) {
    if (s.device_id == device_id) ordered.push_back(&s);
  }
  if (ordered.empty()) return 0.0;
  std::sort(ordered.begin(), ordered.end(), earlier);
  return fold_device(device_id, ordered).duty_cycle;
}

FleetActivity analyze_fleet(const std::vector<DeviceActivity>& per_device) {
  FleetActivity f;
  if (per_device.empty()) return f;

  f.has_devices = true;
  f.device_count = per_device.size();
  f.earliest_begin_ts = per_device.front().first_begin_ts;
  f.latest_end_ts = per_device.front().last_end_ts;

  double duty_sum = 0.0;
  for (const DeviceActivity& a : per_device) {
    f.session_count += a.session_count;
    f.total_active_ns += a.total_active_ns;
    f.total_gap_ns += a.total_gap_ns;
    f.total_checkpoints += a.total_checkpoints;
    f.total_reconnects += a.total_reconnects;
    f.total_markers += a.total_markers;
    f.unclosed_sessions += a.unclosed_sessions;
    f.total_overlaps += a.overlaps;

    if (a.first_begin_ts < f.earliest_begin_ts) {
      f.earliest_begin_ts = a.first_begin_ts;
    }
    if (a.last_end_ts > f.latest_end_ts) f.latest_end_ts = a.last_end_ts;

    if (a.total_active_ns > f.busiest_active_ns) {
      f.busiest_active_ns = a.total_active_ns;
      f.busiest_device = a.device_id;
    }
    duty_sum += a.duty_cycle;
  }

  const uint64_t span = f.total_active_ns + f.total_gap_ns;
  if (span > 0) {
    f.aggregate_duty_cycle =
        static_cast<double>(f.total_active_ns) / static_cast<double>(span);
  }
  f.mean_duty_cycle = duty_sum / static_cast<double>(f.device_count);
  return f;
}

bool longest_idle_gap(const std::vector<Session>& sessions, SessionGap& out) {
  const std::vector<SessionGap> gaps = session_gaps(sessions);
  if (gaps.empty()) return false;
  const SessionGap* best = &gaps.front();
  for (const SessionGap& g : gaps) {
    if (g.gap_ns > best->gap_ns) best = &g;
  }
  out = *best;
  return true;
}

std::string describe_activity(const DeviceActivity& a) {
  std::string out = "device " + hex8(a.device_id) + ": " +
                    std::to_string(a.session_count) + " sessions [" +
                    std::to_string(a.first_begin_ts) + ".." +
                    std::to_string(a.last_end_ts) + "], active " +
                    std::to_string(a.total_active_ns) + "ns, idle " +
                    std::to_string(a.total_gap_ns) + "ns, duty " +
                    duty_str(a.duty_cycle) + ", " +
                    std::to_string(a.total_checkpoints) + " checkpoints, " +
                    std::to_string(a.total_reconnects) + " reconnects, " +
                    std::to_string(a.unclosed_sessions) + " unclosed";
  if (a.overlaps) out += ", " + std::to_string(a.overlaps) + " overlaps";
  if (a.seq_regressions || a.ts_regressions) {
    out += ", " + std::to_string(a.seq_regressions) + " seq / " +
           std::to_string(a.ts_regressions) + " ts regressions";
  }
  return out;
}

std::string describe_fleet(const FleetActivity& f) {
  if (!f.has_devices) return "fleet: 0 devices";
  std::string out =
      "fleet: " + std::to_string(f.device_count) + " devices, " +
      std::to_string(f.session_count) + " sessions [" +
      std::to_string(f.earliest_begin_ts) + ".." +
      std::to_string(f.latest_end_ts) + "], active " +
      std::to_string(f.total_active_ns) + "ns, idle " +
      std::to_string(f.total_gap_ns) + "ns, duty " +
      duty_str(f.aggregate_duty_cycle) + " agg / " +
      duty_str(f.mean_duty_cycle) + " mean, busiest device " +
      hex8(f.busiest_device) + " (" + std::to_string(f.busiest_active_ns) +
      "ns), " + std::to_string(f.total_checkpoints) + " checkpoints, " +
      std::to_string(f.total_reconnects) + " reconnects, " +
      std::to_string(f.unclosed_sessions) + " unclosed";
  if (f.total_overlaps) {
    out += ", " + std::to_string(f.total_overlaps) + " overlaps";
  }
  return out;
}

}
