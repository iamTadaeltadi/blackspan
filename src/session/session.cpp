#include "blackspan/session.h"

#include <cstdio>
#include <utility>

namespace blackspan {

namespace {

// True for record type bytes that the recorder frame format defines. An
// out-of-range byte (a record type this build does not know) returns false.
bool is_known_record_type(RecordType t) {
  switch (t) {
    case RecordType::SessionBegin:
    case RecordType::SessionEnd:
    case RecordType::SchemaDef:
    case RecordType::SampleBlock:
    case RecordType::StateTransition:
    case RecordType::ConfigChange:
    case RecordType::Fault:
    case RecordType::Marker:
    case RecordType::Checkpoint:
    case RecordType::Reconnect:
    case RecordType::DerivedDef:
    case RecordType::ExportRequest:
      return true;
  }
  return false;
}

std::string u32_dec(uint32_t v) { return std::to_string(v); }

std::string u64_dec(uint64_t v) {
  return std::to_string(static_cast<unsigned long long>(v));
}

std::string byte_hex(uint8_t v) {
  char b[8];
  std::snprintf(b, sizeof(b), "0x%02x", static_cast<unsigned>(v));
  return std::string(b);
}

}  // namespace

const char* session_diag_str(SessionDiagCode c) {
  switch (c) {
    case SessionDiagCode::EndWithoutBegin:
      return "end-without-begin";
    case SessionDiagCode::BeginWhileActive:
      return "begin-while-active";
    case SessionDiagCode::ReconnectWhileIdle:
      return "reconnect-while-idle";
    case SessionDiagCode::CheckpointWhileIdle:
      return "checkpoint-while-idle";
    case SessionDiagCode::MarkerWhileIdle:
      return "marker-while-idle";
    case SessionDiagCode::SeqRegression:
      return "seq-regression";
    case SessionDiagCode::TsRegression:
      return "ts-regression";
    case SessionDiagCode::UnterminatedSession:
      return "unterminated-session";
    case SessionDiagCode::UnknownRecordInSession:
      return "unknown-record";
    case SessionDiagCode::TruncatedPayload:
      return "truncated-payload";
  }
  return "unknown";
}

void SessionReconstructor::diag(SessionDiagCode c, uint32_t dev, uint32_t seq,
                                std::string detail) {
  SessionDiagnostic d;
  d.code = c;
  d.device_id = dev;
  d.session_seq = seq;
  d.detail = std::move(detail);
  diags_.push_back(std::move(d));
}

void SessionReconstructor::track_monotonic(DeviceState& ds, const Frame& f) {
  if (!ds.has_active) return;
  Session& s = sessions_[ds.active_index];
  if (ds.have_last) {
    if (f.header.session_seq < ds.last_seq) {
      ++s.seq_regressions;
      diag(SessionDiagCode::SeqRegression, s.device_id, f.header.session_seq,
           "session_seq " + u32_dec(f.header.session_seq) + " < previous " +
               u32_dec(ds.last_seq));
    }
    if (f.header.monotonic_ts < ds.last_ts) {
      ++s.ts_regressions;
      diag(SessionDiagCode::TsRegression, s.device_id, f.header.session_seq,
           "monotonic_ts " + u64_dec(f.header.monotonic_ts) + " < previous " +
               u64_dec(ds.last_ts));
    }
  }
  ds.last_seq = f.header.session_seq;
  ds.last_ts = f.header.monotonic_ts;
  ds.have_last = true;
}

void SessionReconstructor::close_unterminated(DeviceState& ds) {
  Session& s = sessions_[ds.active_index];
  s.end_seq = ds.have_last ? ds.last_seq : s.begin_seq;
  s.end_ts = ds.have_last ? ds.last_ts : s.begin_ts;
  s.closed = false;
  ds.has_active = false;
  ds.active_index = kNoSession;
  ds.have_last = false;
}

void SessionReconstructor::handle_begin(DeviceState& ds, uint32_t dev,
                                        const Frame& f) {
  if (ds.has_active) {
    diag(SessionDiagCode::BeginWhileActive, dev, f.header.session_seq,
         "session #" + u32_dec(sessions_[ds.active_index].session_index) +
             " auto-closed by a new session-begin");
    close_unterminated(ds);
  }

  SessionBegin rec;
  const PayloadStatus ps =
      decode_session_begin(f.payload.data(), f.payload.size(), rec);

  Session s;
  s.device_id = dev;
  s.session_index = ds.session_count++;
  s.device_class = rec.device_class;
  s.fw_major = rec.fw_major;
  s.fw_minor = rec.fw_minor;
  s.fw_patch = rec.fw_patch;
  s.schema_epoch = rec.schema_epoch;
  s.label = rec.label;
  s.begin_seq = f.header.session_seq;
  s.begin_ts = f.header.monotonic_ts;
  s.end_seq = f.header.session_seq;
  s.end_ts = f.header.monotonic_ts;
  sessions_.push_back(std::move(s));

  ds.active_index = sessions_.size() - 1;
  ds.has_active = true;
  ds.last_seq = f.header.session_seq;
  ds.last_ts = f.header.monotonic_ts;
  ds.have_last = true;

  if (ps != PayloadStatus::Ok) {
    diag(SessionDiagCode::TruncatedPayload, dev, f.header.session_seq,
         "session-begin payload truncated");
  }
}

void SessionReconstructor::handle_end(DeviceState& ds, uint32_t dev,
                                      const Frame& f) {
  if (!ds.has_active) {
    diag(SessionDiagCode::EndWithoutBegin, dev, f.header.session_seq,
         "session-end with no active session");
    return;
  }

  SessionEnd rec;
  const PayloadStatus ps =
      decode_session_end(f.payload.data(), f.payload.size(), rec);
  track_monotonic(ds, f);

  Session& s = sessions_[ds.active_index];
  s.end_reason = rec.reason_code;
  s.end_seq = f.header.session_seq;
  s.end_ts = f.header.monotonic_ts;
  s.closed = true;

  if (ps != PayloadStatus::Ok) {
    diag(SessionDiagCode::TruncatedPayload, dev, f.header.session_seq,
         "session-end payload truncated");
  }

  ds.has_active = false;
  ds.active_index = kNoSession;
  ds.have_last = false;
}

void SessionReconstructor::handle_checkpoint(DeviceState& ds, uint32_t dev,
                                             const Frame& f) {
  if (!ds.has_active) {
    diag(SessionDiagCode::CheckpointWhileIdle, dev, f.header.session_seq,
         "checkpoint with no active session");
    return;
  }

  Checkpoint rec;
  const PayloadStatus ps =
      decode_checkpoint(f.payload.data(), f.payload.size(), rec);
  track_monotonic(ds, f);

  sessions_[ds.active_index].checkpoints.push_back(rec);

  if (ps != PayloadStatus::Ok) {
    diag(SessionDiagCode::TruncatedPayload, dev, f.header.session_seq,
         "checkpoint payload truncated");
  }
}

void SessionReconstructor::handle_reconnect(DeviceState& ds, uint32_t dev,
                                            const Frame& f) {
  if (!ds.has_active) {
    diag(SessionDiagCode::ReconnectWhileIdle, dev, f.header.session_seq,
         "reconnect with no active session");
    return;
  }

  Reconnect rec;
  const PayloadStatus ps =
      decode_reconnect(f.payload.data(), f.payload.size(), rec);
  track_monotonic(ds, f);

  Session& s = sessions_[ds.active_index];
  ReconnectEvent ev;
  ev.gap_ms = rec.gap_ms;
  ev.prev_session_seq = rec.prev_session_seq;
  ev.at_seq = f.header.session_seq;
  ev.at_ts = f.header.monotonic_ts;
  s.reconnects.push_back(ev);
  ++s.reconnect_count;

  if (ps != PayloadStatus::Ok) {
    diag(SessionDiagCode::TruncatedPayload, dev, f.header.session_seq,
         "reconnect payload truncated");
  }
}

void SessionReconstructor::handle_marker(DeviceState& ds, uint32_t dev,
                                         const Frame& f) {
  if (!ds.has_active) {
    diag(SessionDiagCode::MarkerWhileIdle, dev, f.header.session_seq,
         "marker with no active session");
    return;
  }

  Marker rec;
  const PayloadStatus ps =
      decode_marker(f.payload.data(), f.payload.size(), rec);
  track_monotonic(ds, f);

  sessions_[ds.active_index].markers.push_back(std::move(rec));

  if (ps != PayloadStatus::Ok) {
    diag(SessionDiagCode::TruncatedPayload, dev, f.header.session_seq,
         "marker payload truncated");
  }
}

void SessionReconstructor::accept(const Frame& f) {
  const RecordType t = f.header.type;
  const uint32_t dev = f.header.device_id;
  DeviceState& ds = devices_[dev];

  if (!is_known_record_type(t)) {
    diag(SessionDiagCode::UnknownRecordInSession, dev, f.header.session_seq,
         "unrecognized record type " +
             byte_hex(static_cast<uint8_t>(t)));
    track_monotonic(ds, f);
    return;
  }

  switch (t) {
    case RecordType::SessionBegin:
      handle_begin(ds, dev, f);
      break;
    case RecordType::SessionEnd:
      handle_end(ds, dev, f);
      break;
    case RecordType::Checkpoint:
      handle_checkpoint(ds, dev, f);
      break;
    case RecordType::Reconnect:
      handle_reconnect(ds, dev, f);
      break;
    case RecordType::Marker:
      handle_marker(ds, dev, f);
      break;
    default:
      // A known record owned by another subsystem. It still advances the
      // session timeline, so it participates in ordering checks.
      track_monotonic(ds, f);
      break;
  }
}

void SessionReconstructor::finish() {
  for (auto& kv : devices_) {
    DeviceState& ds = kv.second;
    if (!ds.has_active) continue;
    Session& s = sessions_[ds.active_index];
    diag(SessionDiagCode::UnterminatedSession, s.device_id, s.begin_seq,
         "session #" + u32_dec(s.session_index) +
             " left open at end of stream");
    close_unterminated(ds);
  }
}

void SessionReconstructor::reconstruct(const uint8_t* data, size_t len) {
  FrameStream fs(data, len);
  Frame f;
  while (fs.next(f)) accept(f);
  finish();
}

SessionSummary SessionReconstructor::summary() const {
  SessionSummary out;
  out.device_count = devices_.size();
  out.total_sessions = sessions_.size();
  out.diagnostic_count = diags_.size();
  for (const Session& s : sessions_) {
    if (s.closed) {
      ++out.closed_sessions;
    } else {
      ++out.unterminated_sessions;
    }
    out.total_checkpoints += s.checkpoints.size();
    out.total_reconnects += s.reconnects.size();
    out.total_markers += s.markers.size();
    out.total_seq_regressions += s.seq_regressions;
    out.total_ts_regressions += s.ts_regressions;
  }
  return out;
}

std::string describe_session(const Session& s) {
  std::string out;
  out += "device ";
  {
    char b[16];
    std::snprintf(b, sizeof(b), "%08x", s.device_id);
    out += b;
  }
  out += " session #" + u32_dec(s.session_index);
  out += " fw " + u32_dec(s.fw_major) + "." + u32_dec(s.fw_minor) + "." +
         u32_dec(s.fw_patch);
  out += " class " + u32_dec(s.device_class);
  out += " epoch " + u32_dec(s.schema_epoch);
  if (!s.label.empty()) out += " \"" + s.label + "\"";
  out += " seq " + u32_dec(s.begin_seq) + ".." + u32_dec(s.end_seq);
  out += s.closed ? " ended" : " unterminated";
  if (s.closed) out += " reason " + u32_dec(s.end_reason);
  out += " duration " + u64_dec(s.duration_ns()) + "ns";
  out += " (" + std::to_string(s.checkpoints.size()) + " checkpoints, " +
         std::to_string(s.reconnects.size()) + " reconnects, " +
         std::to_string(s.markers.size()) + " markers";
  if (s.seq_regressions || s.ts_regressions) {
    out += ", " + u32_dec(s.seq_regressions) + " seq / " +
           u32_dec(s.ts_regressions) + " ts regressions";
  }
  out += ")";
  return out;
}

std::string describe_summary(const SessionSummary& s) {
  std::string out;
  out += std::to_string(s.total_sessions) + " sessions across " +
         std::to_string(s.device_count) + " devices (" +
         std::to_string(s.closed_sessions) + " closed, " +
         std::to_string(s.unterminated_sessions) + " unterminated); " +
         std::to_string(s.total_checkpoints) + " checkpoints, " +
         std::to_string(s.total_reconnects) + " reconnects, " +
         std::to_string(s.total_markers) + " markers; " +
         std::to_string(s.total_seq_regressions) + " seq / " +
         std::to_string(s.total_ts_regressions) + " ts regressions; " +
         std::to_string(s.diagnostic_count) + " diagnostics";
  return out;
}

}  // namespace blackspan
