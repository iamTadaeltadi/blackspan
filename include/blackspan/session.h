#ifndef BLACKSPAN_SESSION_H
#define BLACKSPAN_SESSION_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "blackspan/frame.h"

namespace blackspan {

enum class PayloadStatus {
  Ok,
  Truncated,
};

const char* payload_status_str(PayloadStatus s);

struct SessionBegin {
  uint16_t device_class = 0;
  uint8_t fw_major = 0;
  uint8_t fw_minor = 0;
  uint16_t fw_patch = 0;
  uint32_t schema_epoch = 0;
  std::string label;
};

struct SessionEnd {
  uint16_t reason_code = 0;
};

struct Checkpoint {
  uint32_t checkpoint_id = 0;
  uint32_t bytes_since_last = 0;
};

struct Reconnect {
  uint32_t gap_ms = 0;
  uint32_t prev_session_seq = 0;
};

struct Marker {
  uint8_t marker_kind = 0;
  std::string text;
};

struct ReconnectEvent {
  uint32_t gap_ms = 0;
  uint32_t prev_session_seq = 0;
  uint32_t at_seq = 0;
  uint64_t at_ts = 0;

  bool resumed_from_checkpoint = false;
  uint32_t resume_marker_index = 0;
};

struct CheckpointEvent {
  uint32_t checkpoint_id = 0;
  uint32_t bytes_since_last = 0;
  uint32_t at_seq = 0;
  uint64_t at_ts = 0;
};

struct MarkerEvent {
  uint8_t marker_kind = 0;
  std::string text;
  uint32_t at_seq = 0;
  uint64_t at_ts = 0;
};

PayloadStatus decode_session_begin(const uint8_t* data, size_t len,
                                   SessionBegin& out);
PayloadStatus decode_session_end(const uint8_t* data, size_t len,
                                 SessionEnd& out);
PayloadStatus decode_checkpoint(const uint8_t* data, size_t len,
                                Checkpoint& out);
PayloadStatus decode_reconnect(const uint8_t* data, size_t len, Reconnect& out);
PayloadStatus decode_marker(const uint8_t* data, size_t len, Marker& out);

struct Session {
  uint32_t device_id = 0;
  uint32_t session_index = 0;

  uint16_t device_class = 0;
  uint8_t fw_major = 0;
  uint8_t fw_minor = 0;
  uint16_t fw_patch = 0;
  uint32_t schema_epoch = 0;
  std::string label;

  uint32_t begin_seq = 0;
  uint64_t begin_ts = 0;
  uint32_t end_seq = 0;
  uint64_t end_ts = 0;

  uint16_t end_reason = 0;
  bool closed = false;

  std::vector<CheckpointEvent> checkpoints;
  std::vector<ReconnectEvent> reconnects;
  std::vector<MarkerEvent> markers;

  uint32_t reconnect_count = 0;
  uint32_t seq_regressions = 0;
  uint32_t ts_regressions = 0;

  uint64_t duration_ns() const {
    if (closed && end_ts >= begin_ts) return end_ts - begin_ts;
    return 0;
  }
};

enum class SessionDiagCode {
  EndWithoutBegin,
  BeginWhileActive,
  ReconnectWhileIdle,
  CheckpointWhileIdle,
  MarkerWhileIdle,
  SeqRegression,
  TsRegression,
  UnterminatedSession,
  UnknownRecordInSession,
  TruncatedPayload,
};

const char* session_diag_str(SessionDiagCode c);

struct SessionDiagnostic {
  SessionDiagCode code;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  std::string detail;
};

struct SessionSummary {
  size_t device_count = 0;
  size_t total_sessions = 0;
  size_t closed_sessions = 0;
  size_t unterminated_sessions = 0;
  size_t total_checkpoints = 0;
  size_t total_reconnects = 0;
  size_t total_markers = 0;
  size_t total_seq_regressions = 0;
  size_t total_ts_regressions = 0;
  size_t diagnostic_count = 0;
};

std::string describe_session(const Session& s);
std::string describe_summary(const SessionSummary& s);

class SessionReconstructor {
 public:
  void accept(const Frame& f);
  void finish();

  void reconstruct(const uint8_t* data, size_t len);

  const std::vector<Session>& sessions() const { return sessions_; }
  const std::vector<SessionDiagnostic>& diagnostics() const { return diags_; }
  SessionSummary summary() const;

 private:
  static constexpr size_t kNoSession = static_cast<size_t>(-1);

  struct DeviceState {
    size_t active_index = kNoSession;
    bool has_active = false;
    uint32_t session_count = 0;
    uint32_t last_seq = 0;
    uint64_t last_ts = 0;
    bool have_last = false;

    size_t ckpt_marker_count = 0;
    bool have_ckpt = false;
  };

  void handle_begin(DeviceState& ds, uint32_t dev, const Frame& f);
  void handle_end(DeviceState& ds, uint32_t dev, const Frame& f);
  void handle_checkpoint(DeviceState& ds, uint32_t dev, const Frame& f);
  void handle_reconnect(DeviceState& ds, uint32_t dev, const Frame& f);
  void handle_marker(DeviceState& ds, uint32_t dev, const Frame& f);

  void track_monotonic(DeviceState& ds, const Frame& f);
  void close_unterminated(DeviceState& ds);
  void diag(SessionDiagCode c, uint32_t dev, uint32_t seq, std::string detail);

  std::vector<Session> sessions_;
  std::vector<SessionDiagnostic> diags_;
  std::map<uint32_t, DeviceState> devices_;
};

}

#endif
