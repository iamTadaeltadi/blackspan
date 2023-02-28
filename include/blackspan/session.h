#ifndef BLACKSPAN_SESSION_H
#define BLACKSPAN_SESSION_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "blackspan/frame.h"

// The session subsystem reconstructs recorder sessions from a decoded frame
// stream. It owns the payload grammars of the session-control record types
// (session-begin/end, checkpoint, reconnect, marker), turns them into typed
// records, and runs a per-device state machine that groups frames into
// Session objects while reporting ordering and structural anomalies as
// diagnostics rather than errors. Nothing here throws: malformed or misordered
// input is recorded and decoding continues.

namespace blackspan {

// Result of decoding a record payload. A payload that runs out of bytes before
// its declared fields are satisfied is reported as Truncated; whatever fields
// decoded before the short read are still written to the output struct.
enum class PayloadStatus {
  Ok,
  Truncated,
};

const char* payload_status_str(PayloadStatus s);

// session-begin (0x01):
//   [device_class u16][fw_major u8][fw_minor u8][fw_patch u16]
//   [schema_epoch u32][label_len u8][label bytes]
struct SessionBegin {
  uint16_t device_class = 0;
  uint8_t fw_major = 0;
  uint8_t fw_minor = 0;
  uint16_t fw_patch = 0;
  uint32_t schema_epoch = 0;
  std::string label;
};

// session-end (0x02): [reason_code u16]
struct SessionEnd {
  uint16_t reason_code = 0;
};

// checkpoint (0x09): [checkpoint_id u32][bytes_since_last u32]
struct Checkpoint {
  uint32_t checkpoint_id = 0;
  uint32_t bytes_since_last = 0;
};

// reconnect (0x0A): [gap_ms u32][prev_session_seq u32]
struct Reconnect {
  uint32_t gap_ms = 0;
  uint32_t prev_session_seq = 0;
};

// marker (0x08): [marker_kind u8][text_len u8][text bytes]
struct Marker {
  uint8_t marker_kind = 0;
  std::string text;
};

// A reconnect as attached to a session: the decoded payload plus the frame
// position (session_seq / monotonic_ts) at which the recorder resumed.
struct ReconnectEvent {
  uint32_t gap_ms = 0;
  uint32_t prev_session_seq = 0;
  uint32_t at_seq = 0;
  uint64_t at_ts = 0;
};

// A checkpoint as attached to a session: the decoded payload plus the frame
// position it was recorded at. The wire payload carries no timestamp of its
// own, so the frame's session_seq / monotonic_ts locate it on the timeline.
struct CheckpointEvent {
  uint32_t checkpoint_id = 0;
  uint32_t bytes_since_last = 0;
  uint32_t at_seq = 0;
  uint64_t at_ts = 0;
};

// A marker as attached to a session: the decoded payload plus the frame
// position it was recorded at.
struct MarkerEvent {
  uint8_t marker_kind = 0;
  std::string text;
  uint32_t at_seq = 0;
  uint64_t at_ts = 0;
};

// Typed decoders for the payload grammars owned by this subsystem. Each reads
// little-endian via ByteReader and is robust to truncation.
PayloadStatus decode_session_begin(const uint8_t* data, size_t len,
                                   SessionBegin& out);
PayloadStatus decode_session_end(const uint8_t* data, size_t len,
                                 SessionEnd& out);
PayloadStatus decode_checkpoint(const uint8_t* data, size_t len,
                                Checkpoint& out);
PayloadStatus decode_reconnect(const uint8_t* data, size_t len, Reconnect& out);
PayloadStatus decode_marker(const uint8_t* data, size_t len, Marker& out);

// A single reconstructed recorder session for one device.
struct Session {
  uint32_t device_id = 0;
  uint32_t session_index = 0;  // 0-based ordinal within this device

  // Device metadata carried by the opening session-begin record.
  uint16_t device_class = 0;
  uint8_t fw_major = 0;
  uint8_t fw_minor = 0;
  uint16_t fw_patch = 0;
  uint32_t schema_epoch = 0;
  std::string label;

  // Frame position of the opening record and of the close.
  uint32_t begin_seq = 0;
  uint64_t begin_ts = 0;
  uint32_t end_seq = 0;
  uint64_t end_ts = 0;

  uint16_t end_reason = 0;  // reason_code from session-end; 0 if unterminated
  bool closed = false;      // true only when ended by a session-end record

  std::vector<CheckpointEvent> checkpoints;
  std::vector<ReconnectEvent> reconnects;
  std::vector<MarkerEvent> markers;

  uint32_t reconnect_count = 0;
  uint32_t seq_regressions = 0;  // frames whose session_seq went backwards
  uint32_t ts_regressions = 0;   // frames whose monotonic_ts went backwards

  // Elapsed device-monotonic time, defined only for a properly closed session
  // whose end timestamp is not before its begin timestamp; 0 otherwise.
  uint64_t duration_ns() const {
    if (closed && end_ts >= begin_ts) return end_ts - begin_ts;
    return 0;
  }
};

enum class SessionDiagCode {
  EndWithoutBegin,         // session-end with no active session
  BeginWhileActive,        // session-begin while a session was still open
  ReconnectWhileIdle,      // reconnect outside any session
  CheckpointWhileIdle,     // checkpoint outside any session
  MarkerWhileIdle,         // marker outside any session
  SeqRegression,           // session_seq moved backwards within a session
  TsRegression,            // monotonic_ts moved backwards within a session
  UnterminatedSession,     // session left open at end of stream
  UnknownRecordInSession,  // record type not recognized by the format
  TruncatedPayload,        // a record payload was shorter than its grammar
};

const char* session_diag_str(SessionDiagCode c);

struct SessionDiagnostic {
  SessionDiagCode code;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  std::string detail;
};

// Aggregate counts across everything a reconstructor has processed.
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

// Groups a decoded frame stream into sessions. Sessions are tracked
// independently per device_id, so interleaved recordings from several devices
// reconstruct correctly. Feed frames with accept() (in stream order) and call
// finish() once the stream is exhausted, or use reconstruct() for a one-shot
// pass over a raw buffer.
class SessionReconstructor {
 public:
  void accept(const Frame& f);
  void finish();

  // Decodes data[0, len) as a frame stream, feeding each frame to accept(),
  // then calls finish(). A frame stream that stops early (corrupt or partial
  // frame) simply ends the pass; sessions still open are finalized.
  void reconstruct(const uint8_t* data, size_t len);

  const std::vector<Session>& sessions() const { return sessions_; }
  const std::vector<SessionDiagnostic>& diagnostics() const { return diags_; }
  SessionSummary summary() const;

 private:
  static constexpr size_t kNoSession = static_cast<size_t>(-1);

  struct DeviceState {
    size_t active_index = kNoSession;  // index into sessions_ while open
    bool has_active = false;
    uint32_t session_count = 0;  // sessions begun for this device so far
    uint32_t last_seq = 0;       // last seq observed within the open session
    uint64_t last_ts = 0;        // last ts observed within the open session
    bool have_last = false;
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

}  // namespace blackspan

#endif  // BLACKSPAN_SESSION_H
