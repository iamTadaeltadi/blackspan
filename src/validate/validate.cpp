#include <algorithm>
#include <cstdio>
#include <map>

#include "blackspan/validate.h"

namespace blackspan {
namespace {

Severity3 severity_of(ValidationCode c) {
  switch (c) {
    case ValidationCode::StreamHalted:
    case ValidationCode::EndWithoutBegin:
    case ValidationCode::ChannelSampledUndeclared:
    case ValidationCode::FaultUnknownDevice:
      return Severity3::Error;

    case ValidationCode::StreamTruncated:
    case ValidationCode::EmptyRecording:
    case ValidationCode::UnterminatedSession:
    case ValidationCode::SessionSeqRegression:
    case ValidationCode::SessionTsRegression:
    case ValidationCode::ChannelEmptySeries:
    case ValidationCode::OrphanConfigChange:
      return Severity3::Warning;

    case ValidationCode::SessionNoActivity:
    case ValidationCode::ZeroDurationSession:
    case ValidationCode::ChannelDeclaredUnused:
    case ValidationCode::SchemaMultipleVersions:
    case ValidationCode::StateDesync:
    case ValidationCode::CriticalFault:
    case ValidationCode::FaultBurst:
    case ValidationCode::FaultWithoutState:
      return Severity3::Note;
  }
  return Severity3::Note;
}

struct SessionSpan {
  uint32_t begin_seq = 0;
  uint32_t end_seq = 0;
  bool closed = false;
};

class Builder {
 public:
  explicit Builder(const ValidationOptions& opts) : opts_(opts) {}

  void add(ValidationCode code, uint32_t dev, uint32_t seq, std::string detail) {
    Severity3 sev = severity_of(code);
    if (sev == Severity3::Note && !opts_.include_notes) return;
    Finding f;
    f.severity = sev;
    f.code = code;
    f.device_id = dev;
    f.session_seq = seq;
    f.detail = std::move(detail);
    findings_.push_back(std::move(f));
  }

  std::vector<Finding> take() { return std::move(findings_); }

 private:
  const ValidationOptions& opts_;
  std::vector<Finding> findings_;
};

void check_stream(const DecodedRecording& rec, Builder& b) {
  if (rec.frame_count() == 0) {
    b.add(ValidationCode::EmptyRecording, 0, 0, "no complete frames decoded");
    return;
  }
  switch (rec.final_status()) {
    case FrameStatus::Ok:
      break;
    case FrameStatus::Incomplete:
      b.add(ValidationCode::StreamTruncated, 0, 0,
            "recording ends mid-frame after " +
                std::to_string(rec.frame_count()) + " complete frames");
      break;
    default:
      b.add(ValidationCode::StreamHalted, 0, 0,
            std::string("decode halted: ") +
                frame_status_str(rec.final_status()));
      break;
  }
}

std::map<uint32_t, std::vector<SessionSpan>> build_spans(
    const std::vector<Session>& sessions) {
  std::map<uint32_t, std::vector<SessionSpan>> spans;
  for (const Session& s : sessions) {
    SessionSpan sp;
    sp.begin_seq = s.begin_seq;
    sp.end_seq = s.end_seq;
    sp.closed = s.closed;
    spans[s.device_id].push_back(sp);
  }
  return spans;
}

bool seq_in_any_span(const std::map<uint32_t, std::vector<SessionSpan>>& spans,
                     uint32_t dev, uint32_t seq) {
  auto it = spans.find(dev);
  if (it == spans.end()) return false;
  for (const SessionSpan& sp : it->second) {
    if (seq >= sp.begin_seq && seq <= sp.end_seq) return true;
  }
  return false;
}

void check_sessions(const DecodedRecording& rec, Builder& b) {
  const std::vector<Session>& sessions = rec.sessions().sessions();

  std::map<uint32_t, std::vector<uint32_t>> activity;
  for (const FaultEvent& f : rec.anomalies().faults())
    activity[f.device_id].push_back(f.session_seq);
  for (const TransitionEvent& t : rec.anomalies().transitions())
    activity[t.device_id].push_back(t.session_seq);
  for (const ConfigEvent& c : rec.anomalies().config_changes())
    activity[c.device_id].push_back(c.session_seq);

  for (const Session& s : sessions) {
    if (!s.closed) {
      b.add(ValidationCode::UnterminatedSession, s.device_id, s.begin_seq,
            "session #" + std::to_string(s.session_index) +
                " left open at end of stream");
    }
    if (s.seq_regressions) {
      b.add(ValidationCode::SessionSeqRegression, s.device_id, s.begin_seq,
            std::to_string(s.seq_regressions) +
                " session_seq regression(s) in session #" +
                std::to_string(s.session_index));
    }
    if (s.ts_regressions) {
      b.add(ValidationCode::SessionTsRegression, s.device_id, s.begin_seq,
            std::to_string(s.ts_regressions) +
                " monotonic_ts regression(s) in session #" +
                std::to_string(s.session_index));
    }
    if (s.closed && s.end_ts == s.begin_ts) {
      b.add(ValidationCode::ZeroDurationSession, s.device_id, s.begin_seq,
            "session #" + std::to_string(s.session_index) +
                " began and ended at ts " + std::to_string(s.begin_ts));
    }

    bool has_side_records = !s.checkpoints.empty() || !s.markers.empty() ||
                            !s.reconnects.empty();
    bool has_activity = has_side_records;
    if (!has_activity) {
      auto it = activity.find(s.device_id);
      if (it != activity.end()) {
        for (uint32_t seq : it->second) {
          if (seq >= s.begin_seq && seq <= s.end_seq) {
            has_activity = true;
            break;
          }
        }
      }
    }
    if (!has_activity) {
      b.add(ValidationCode::SessionNoActivity, s.device_id, s.begin_seq,
            "session #" + std::to_string(s.session_index) +
                " carried no checkpoints, markers, faults, or transitions");
    }
  }

  for (const SessionDiagnostic& d : rec.sessions().diagnostics()) {
    if (d.code == SessionDiagCode::EndWithoutBegin) {
      b.add(ValidationCode::EndWithoutBegin, d.device_id, d.session_seq,
            d.detail);
    }
  }
}

void check_channels(const DecodedRecording& rec, Builder& b) {
  const SchemaRegistry& schemas = rec.schemas();
  const ChannelStore& channels = rec.channels();

  for (uint16_t id : schemas.channels()) {
    const ChannelSeries* series = channels.series(id);
    if (series == nullptr) {
      b.add(ValidationCode::ChannelDeclaredUnused, 0, 0,
            "channel " + std::to_string(id) +
                " was declared but never sampled");
      continue;
    }
    if (series->total_samples() == 0) {
      b.add(ValidationCode::ChannelEmptySeries, 0, 0,
            "channel " + std::to_string(id) + " has a series with no samples");
    }
    if (schemas.versions(id).size() > 1) {
      b.add(ValidationCode::SchemaMultipleVersions, 0, 0,
            "channel " + std::to_string(id) + " changed layout across " +
                std::to_string(schemas.versions(id).size()) + " versions");
    }
  }

  for (uint16_t id : channels.channels()) {
    if (!schemas.contains(id)) {
      b.add(ValidationCode::ChannelSampledUndeclared, 0, 0,
            "channel " + std::to_string(id) +
                " has samples with no registered schema");
    }
  }
}

void check_anomalies(const DecodedRecording& rec, Builder& b,
                     const ValidationOptions& opts) {
  const AnomalyEngine& an = rec.anomalies();

  for (const TransitionEvent& t : an.transitions()) {
    if (!t.continues_chain) {
      b.add(ValidationCode::StateDesync, t.device_id, t.session_seq,
            "transition s" + std::to_string(t.from_state) + " -> s" +
                std::to_string(t.to_state) + " did not continue the chain");
    }
  }

  for (const FaultEvent& f : an.faults()) {
    if (f.severity == Severity::Critical) {
      b.add(ValidationCode::CriticalFault, f.device_id, f.session_seq,
            "critical fault code " + std::to_string(f.fault_code) +
                " subsystem " + std::to_string(f.subsystem_id));
    }
    if (!f.state_at_fault.has_value()) {
      b.add(ValidationCode::FaultWithoutState, f.device_id, f.session_seq,
            "fault code " + std::to_string(f.fault_code) +
                " fired before any state transition");
    }
  }

  std::vector<BurstWindow> bursts =
      an.burst_windows(opts.burst_window_ns, opts.burst_min_count);
  for (const BurstWindow& w : bursts) {
    b.add(ValidationCode::FaultBurst, w.device_id, w.first_seq,
          std::to_string(w.count) + " faults within " +
              std::to_string(w.last_ts - w.first_ts) + " ns");
  }
}

void check_cross(const DecodedRecording& rec, Builder& b) {
  const std::vector<Session>& sessions = rec.sessions().sessions();
  std::map<uint32_t, std::vector<SessionSpan>> spans = build_spans(sessions);

  for (const FaultEvent& f : rec.anomalies().faults()) {
    if (spans.find(f.device_id) == spans.end()) {
      b.add(ValidationCode::FaultUnknownDevice, f.device_id, f.session_seq,
            "fault code " + std::to_string(f.fault_code) +
                " from a device with no reconstructed session");
    }
  }

  for (const ConfigEvent& c : rec.anomalies().config_changes()) {
    if (spans.find(c.device_id) != spans.end() &&
        !seq_in_any_span(spans, c.device_id, c.session_seq)) {
      b.add(ValidationCode::OrphanConfigChange, c.device_id, c.session_seq,
            "config change to param " + std::to_string(c.param_id) +
                " fell outside every session span");
    }
  }
}

}

const char* severity3_name(Severity3 s) {
  switch (s) {
    case Severity3::Note: return "note";
    case Severity3::Warning: return "warning";
    case Severity3::Error: return "error";
  }
  return "unknown";
}

const char* validation_code_name(ValidationCode c) {
  switch (c) {
    case ValidationCode::StreamTruncated: return "stream-truncated";
    case ValidationCode::StreamHalted: return "stream-halted";
    case ValidationCode::EmptyRecording: return "empty-recording";
    case ValidationCode::UnterminatedSession: return "unterminated-session";
    case ValidationCode::SessionSeqRegression: return "session-seq-regression";
    case ValidationCode::SessionTsRegression: return "session-ts-regression";
    case ValidationCode::SessionNoActivity: return "session-no-activity";
    case ValidationCode::EndWithoutBegin: return "end-without-begin";
    case ValidationCode::ZeroDurationSession: return "zero-duration-session";
    case ValidationCode::ChannelDeclaredUnused: return "channel-declared-unused";
    case ValidationCode::ChannelSampledUndeclared:
      return "channel-sampled-undeclared";
    case ValidationCode::ChannelEmptySeries: return "channel-empty-series";
    case ValidationCode::SchemaMultipleVersions:
      return "schema-multiple-versions";
    case ValidationCode::StateDesync: return "state-desync";
    case ValidationCode::CriticalFault: return "critical-fault";
    case ValidationCode::FaultBurst: return "fault-burst";
    case ValidationCode::FaultWithoutState: return "fault-without-state";
    case ValidationCode::FaultUnknownDevice: return "fault-unknown-device";
    case ValidationCode::OrphanConfigChange: return "orphan-config-change";
  }
  return "unknown";
}

std::string describe_finding(const Finding& f) {
  char head[64];
  std::snprintf(head, sizeof(head), "%-7s [%s] dev=%08x seq=%u: ",
                severity3_name(f.severity), validation_code_name(f.code),
                f.device_id, f.session_seq);
  std::string s = head;
  s += f.detail;
  return s;
}

ValidationReport ValidationReport::run(const DecodedRecording& rec,
                                       const ValidationOptions& opts) {
  Builder b(opts);

  check_stream(rec, b);
  check_sessions(rec, b);
  check_channels(rec, b);
  check_anomalies(rec, b, opts);
  check_cross(rec, b);

  ValidationReport report;
  report.findings_ = b.take();
  for (const Finding& f : report.findings_) {
    switch (f.severity) {
      case Severity3::Error: ++report.summary_.errors; break;
      case Severity3::Warning: ++report.summary_.warnings; break;
      case Severity3::Note: ++report.summary_.notes; break;
    }
  }
  return report;
}

std::vector<const Finding*> ValidationReport::at_severity(Severity3 s) const {
  std::vector<const Finding*> out;
  for (const Finding& f : findings_) {
    if (f.severity == s) out.push_back(&f);
  }
  return out;
}

size_t ValidationReport::count_of(ValidationCode c) const {
  size_t n = 0;
  for (const Finding& f : findings_) {
    if (f.code == c) ++n;
  }
  return n;
}

std::string render_validation(const ValidationReport& report) {
  if (report.findings().empty()) return "ok: no findings\n";

  std::string out;
  for (const Finding& f : report.findings()) {
    out += describe_finding(f);
    out += "\n";
  }
  const ValidationSummary& s = report.summary();
  out += std::to_string(s.errors) + " error(s), " +
         std::to_string(s.warnings) + " warning(s), " +
         std::to_string(s.notes) + " note(s)\n";
  return out;
}

}
