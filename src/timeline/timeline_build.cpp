#include <algorithm>
#include <cstdio>
#include <map>

#include "blackspan/timeline.h"

namespace blackspan {
namespace {

void collect_session_events(const Session& s, std::vector<TimelineEvent>& out) {
  {
    TimelineEvent e;
    e.monotonic_ts = s.begin_ts;
    e.device_id = s.device_id;
    e.session_seq = s.begin_seq;
    e.kind = TimelineKind::SessionBegin;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "session #%u fw %u.%u.%u class %u \"%s\"",
                  s.session_index, s.fw_major, s.fw_minor, s.fw_patch,
                  s.device_class, s.label.c_str());
    e.detail = buf;
    out.push_back(std::move(e));
  }

  if (s.closed) {
    TimelineEvent e;
    e.monotonic_ts = s.end_ts;
    e.device_id = s.device_id;
    e.session_seq = s.end_seq;
    e.kind = TimelineKind::SessionEnd;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "reason %u", s.end_reason);
    e.detail = buf;
    out.push_back(std::move(e));
  }

  for (const CheckpointEvent& c : s.checkpoints) {
    TimelineEvent e;
    e.monotonic_ts = c.at_ts;
    e.device_id = s.device_id;
    e.session_seq = c.at_seq;
    e.kind = TimelineKind::Checkpoint;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "id %u, %u bytes since last",
                  c.checkpoint_id, c.bytes_since_last);
    e.detail = buf;
    out.push_back(std::move(e));
  }

  for (const ReconnectEvent& r : s.reconnects) {
    TimelineEvent e;
    e.monotonic_ts = r.at_ts;
    e.device_id = s.device_id;
    e.session_seq = r.at_seq;
    e.kind = TimelineKind::Reconnect;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "gap %u ms, prev seq %u", r.gap_ms,
                  r.prev_session_seq);
    e.detail = buf;
    out.push_back(std::move(e));
  }

  for (const MarkerEvent& m : s.markers) {
    TimelineEvent e;
    e.monotonic_ts = m.at_ts;
    e.device_id = s.device_id;
    e.session_seq = m.at_seq;
    e.kind = TimelineKind::Marker;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "kind %u: ", m.marker_kind);
    e.detail = buf;
    e.detail += m.text;
    out.push_back(std::move(e));
  }
}

void collect_anomaly_events(const AnomalyEngine& anomalies,
                            std::vector<TimelineEvent>& out) {
  for (const TransitionEvent& t : anomalies.transitions()) {
    TimelineEvent e;
    e.monotonic_ts = t.monotonic_ts;
    e.device_id = t.device_id;
    e.session_seq = t.session_seq;
    e.kind = TimelineKind::StateTransition;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "s%u -> s%u (cause %u)%s", t.from_state,
                  t.to_state, t.cause,
                  t.continues_chain ? "" : " [desync]");
    e.detail = buf;
    out.push_back(std::move(e));
  }

  for (const ConfigEvent& c : anomalies.config_changes()) {
    TimelineEvent e;
    e.monotonic_ts = c.monotonic_ts;
    e.device_id = c.device_id;
    e.session_seq = c.session_seq;
    e.kind = TimelineKind::ConfigChange;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "param %u: %u -> %u", c.param_id,
                  c.old_value, c.new_value);
    e.detail = buf;
    out.push_back(std::move(e));
  }

  for (const FaultEvent& f : anomalies.faults()) {
    TimelineEvent e;
    e.monotonic_ts = f.monotonic_ts;
    e.device_id = f.device_id;
    e.session_seq = f.session_seq;
    e.kind = TimelineKind::Fault;
    e.severity = f.severity;
    char buf[128];
    if (f.state_at_fault.has_value()) {
      std::snprintf(buf, sizeof(buf), "code %u %s subsystem %u (state s%u)",
                    f.fault_code, severity_name(f.severity), f.subsystem_id,
                    f.state_at_fault.value());
    } else {
      std::snprintf(buf, sizeof(buf), "code %u %s subsystem %u", f.fault_code,
                    severity_name(f.severity), f.subsystem_id);
    }
    e.detail = buf;
    out.push_back(std::move(e));
  }
}

void collect_channel_events(const ChannelStore& channels,
                            std::vector<TimelineEvent>& out) {
  for (uint16_t id : channels.channels()) {
    const ChannelSeries* series = channels.series(id);
    if (series == nullptr) continue;
    for (size_t b = 0; b < series->block_count(); ++b) {
      TimelineEvent e;
      e.monotonic_ts = series->block_base_ts(b);

      e.device_id = 0;
      e.kind = TimelineKind::SampleBlock;
      e.channel_id = id;
      e.count = series->block_sample_count(b);
      char buf[96];
      std::snprintf(buf, sizeof(buf), "channel %u \"%s\" x%u samples", id,
                    series->schema().name.c_str(), e.count);
      e.detail = buf;
      out.push_back(std::move(e));
    }
  }
}

bool passes(const TimelineEvent& e, const TimelineOptions& opts) {
  if (opts.device.has_value() && e.device_id != opts.device.value()) {
    return false;
  }
  if ((opts.kind_mask & timeline_kind_bit(e.kind)) == 0) return false;
  if (opts.since.has_value() && e.monotonic_ts < opts.since.value()) {
    return false;
  }
  if (opts.until.has_value() && e.monotonic_ts > opts.until.value()) {
    return false;
  }
  return true;
}

}

const char* timeline_kind_name(TimelineKind k) {
  switch (k) {
    case TimelineKind::SessionBegin: return "session-begin";
    case TimelineKind::SessionEnd: return "session-end";
    case TimelineKind::Checkpoint: return "checkpoint";
    case TimelineKind::Reconnect: return "reconnect";
    case TimelineKind::Marker: return "marker";
    case TimelineKind::StateTransition: return "state-transition";
    case TimelineKind::ConfigChange: return "config-change";
    case TimelineKind::Fault: return "fault";
    case TimelineKind::SampleBlock: return "sample-block";
  }
  return "unknown";
}

std::string describe_timeline_event(const TimelineEvent& e) {
  char head[64];
  std::snprintf(head, sizeof(head), "ts=%llu dev=%08x %-16s ",
                static_cast<unsigned long long>(e.monotonic_ts), e.device_id,
                timeline_kind_name(e.kind));
  std::string s = head;
  s += e.detail;
  return s;
}

Timeline Timeline::build(const std::vector<Session>& sessions,
                         const AnomalyEngine& anomalies,
                         const ChannelStore* channels,
                         const TimelineOptions& opts) {
  std::vector<TimelineEvent> all;
  for (const Session& s : sessions) collect_session_events(s, all);
  collect_anomaly_events(anomalies, all);
  if (channels != nullptr) collect_channel_events(*channels, all);

  Timeline tl;
  for (TimelineEvent& e : all) {
    if (passes(e, opts)) tl.events_.push_back(std::move(e));
  }

  std::stable_sort(tl.events_.begin(), tl.events_.end(),
                   [](const TimelineEvent& a, const TimelineEvent& b) {
                     if (a.monotonic_ts != b.monotonic_ts) {
                       return a.monotonic_ts < b.monotonic_ts;
                     }
                     return a.session_seq < b.session_seq;
                   });
  return tl;
}

Timeline Timeline::build(const DecodedRecording& rec,
                         const TimelineOptions& opts) {
  return build(rec.sessions().sessions(), rec.anomalies(), &rec.channels(),
               opts);
}

size_t Timeline::count_of(TimelineKind k) const {
  size_t n = 0;
  for (const TimelineEvent& e : events_) {
    if (e.kind == k) ++n;
  }
  return n;
}

std::vector<uint32_t> Timeline::devices() const {
  std::map<uint32_t, bool> seen;
  for (const TimelineEvent& e : events_) seen[e.device_id] = true;
  std::vector<uint32_t> out;
  out.reserve(seen.size());
  for (const auto& kv : seen) out.push_back(kv.first);
  return out;
}

std::optional<std::pair<uint64_t, uint64_t>> Timeline::span() const {
  if (events_.empty()) return std::nullopt;

  return std::make_pair(events_.front().monotonic_ts,
                        events_.back().monotonic_ts);
}

std::vector<const TimelineEvent*> Timeline::in_window(uint64_t lo,
                                                      uint64_t hi) const {
  std::vector<const TimelineEvent*> out;
  if (hi < lo) return out;
  for (const TimelineEvent& e : events_) {
    if (e.monotonic_ts >= lo && e.monotonic_ts <= hi) out.push_back(&e);
  }
  return out;
}

std::vector<size_t> Timeline::density(uint64_t window_ns) const {
  std::vector<size_t> out;
  if (window_ns == 0 || events_.empty()) return out;

  const uint64_t start = events_.front().monotonic_ts;
  const uint64_t end = events_.back().monotonic_ts;
  const uint64_t span = end - start;
  const size_t buckets = static_cast<size_t>(span / window_ns) + 1;
  out.assign(buckets, 0);
  for (const TimelineEvent& e : events_) {
    const uint64_t offset = e.monotonic_ts - start;
    size_t idx = static_cast<size_t>(offset / window_ns);
    if (idx >= buckets) idx = buckets - 1;
    ++out[idx];
  }
  return out;
}

std::string render_timeline(const Timeline& tl) {
  std::string out;
  char head[128];
  auto span = tl.span();
  if (span.has_value()) {
    std::snprintf(head, sizeof(head), "%zu events over ts %llu..%llu\n",
                  tl.size(),
                  static_cast<unsigned long long>(span->first),
                  static_cast<unsigned long long>(span->second));
  } else {
    std::snprintf(head, sizeof(head), "0 events\n");
  }
  out += head;
  for (const TimelineEvent& e : tl.events()) {
    out += "  ";
    out += describe_timeline_event(e);
    out += "\n";
  }
  return out;
}

}
