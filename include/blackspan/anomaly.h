#ifndef BLACKSPAN_ANOMALY_H
#define BLACKSPAN_ANOMALY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "blackspan/frame.h"

// The anomaly subsystem correlates a device's fault reports with its
// state-machine transitions and configuration changes. It owns the payload
// grammars of the state-transition, config-change, and fault record types,
// decodes them into typed events that carry their frame position, tracks a
// per-device state derived from the transition chain, and tags each fault with
// the state that was active when it was recorded. Structural problems (a
// transition that does not continue the known chain, a truncated payload, an
// unrecognized record type) are surfaced as diagnostics rather than errors:
// nothing here throws, and malformed or misordered input is recorded and
// decoding continues.

namespace blackspan {

// Result of decoding a record payload. A payload that runs out of bytes before
// its declared fields are satisfied is reported as Truncated; whatever fields
// decoded before the short read are still written to the output struct.
enum class DecodeStatus {
  Ok,
  Truncated,
};

const char* decode_status_str(DecodeStatus s);

// Severity of a fault, derived from the 1-byte severity field. A byte outside
// the defined range is clamped to Critical (the most severe class) so that a
// corrupt or forward-compatible severity is never under-reported.
enum class Severity : uint8_t {
  Info = 0,
  Warning = 1,
  Error = 2,
  Critical = 3,
};

const char* severity_name(Severity s);

// Maps a raw severity byte to a Severity, clamping anything above Critical.
Severity severity_from_byte(uint8_t v);

// state-transition (0x05):
//   [from_state u16][to_state u16][cause u8][reserved u8]
struct StateTransition {
  uint16_t from_state = 0;
  uint16_t to_state = 0;
  uint8_t cause = 0;
  uint8_t reserved = 0;
};

// config-change (0x06): [param_id u16][old_value u32][new_value u32]
struct ConfigChange {
  uint16_t param_id = 0;
  uint32_t old_value = 0;
  uint32_t new_value = 0;
};

// fault (0x07):
//   [fault_code u16][severity u8][subsystem_id u8][detail_len u8][detail bytes]
struct Fault {
  uint16_t fault_code = 0;
  Severity severity = Severity::Info;
  uint8_t subsystem_id = 0;
  std::vector<uint8_t> detail;
};

// Typed decoders for the payload grammars owned by this subsystem. Each reads
// little-endian via ByteReader and is robust to truncation.
DecodeStatus decode_state_transition(const uint8_t* data, size_t len,
                                     StateTransition& out);
DecodeStatus decode_config_change(const uint8_t* data, size_t len,
                                  ConfigChange& out);
DecodeStatus decode_fault(const uint8_t* data, size_t len, Fault& out);

// A decoded state-transition placed on the timeline: the payload fields plus
// the frame position that carried it, and whether its from_state continued the
// device's known transition chain.
struct TransitionEvent {
  uint16_t from_state = 0;
  uint16_t to_state = 0;
  uint8_t cause = 0;
  uint8_t reserved = 0;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  uint64_t monotonic_ts = 0;
  bool continues_chain = true;  // false when from_state broke the known chain
};

// A decoded config-change placed on the timeline.
struct ConfigEvent {
  uint16_t param_id = 0;
  uint32_t old_value = 0;
  uint32_t new_value = 0;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  uint64_t monotonic_ts = 0;
};

// A decoded fault placed on the timeline, correlated with the device state that
// was active when it was recorded. state_at_fault is empty when the device had
// recorded no transition before the fault.
struct FaultEvent {
  uint16_t fault_code = 0;
  Severity severity = Severity::Info;
  uint8_t subsystem_id = 0;
  std::vector<uint8_t> detail;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  uint64_t monotonic_ts = 0;
  std::optional<uint16_t> state_at_fault;
};

enum class AnomalyDiagCode {
  StateDesync,       // a transition's from_state did not match the known state
  TruncatedPayload,  // a record payload was shorter than its grammar
  UnknownRecord,     // a record type byte the frame format does not define
};

const char* anomaly_diag_str(AnomalyDiagCode c);

struct AnomalyDiagnostic {
  AnomalyDiagCode code;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  std::string detail;
};

// A detected fault burst: a run of one device's faults that all fall within
// window_ns of the run's first fault, containing at least the requested
// minimum count. See AnomalyEngine::burst_windows.
struct BurstWindow {
  uint32_t device_id = 0;
  size_t count = 0;
  uint64_t first_ts = 0;
  uint64_t last_ts = 0;
  uint32_t first_seq = 0;
  uint32_t last_seq = 0;
  std::vector<size_t> member_indices;  // indices into faults(), earliest first
};

// Aggregate counts across everything an engine has processed.
struct AnomalySummary {
  size_t device_count = 0;
  size_t total_faults = 0;
  size_t total_transitions = 0;
  size_t total_config_changes = 0;
  size_t state_desyncs = 0;
  std::array<size_t, 4> faults_by_severity{};  // indexed by Severity
  size_t diagnostic_count = 0;
};

std::string describe_fault(const FaultEvent& f);
std::string describe_transition(const TransitionEvent& t);
std::string describe_config(const ConfigEvent& c);
std::string describe_anomaly_summary(const AnomalySummary& s);

// Correlates faults, state transitions, and config changes decoded from a frame
// stream. State is tracked independently per device_id, so interleaved
// recordings from several devices correlate correctly. Feed frames with
// accept() in stream order, or use ingest() for a one-shot pass over a raw
// buffer.
class AnomalyEngine {
 public:
  void accept(const Frame& f);

  // Decodes data[0, len) as a frame stream, feeding each frame to accept(). A
  // frame stream that stops early (corrupt or partial frame) simply ends the
  // pass; everything decoded so far is retained.
  void ingest(const uint8_t* data, size_t len);

  const std::vector<FaultEvent>& faults() const { return faults_; }
  const std::vector<TransitionEvent>& transitions() const {
    return transitions_;
  }
  const std::vector<ConfigEvent>& config_changes() const { return configs_; }
  const std::vector<AnomalyDiagnostic>& diagnostics() const { return diags_; }

  // Current transition-derived state for a device, or empty if the device has
  // recorded no state transition yet.
  std::optional<uint16_t> current_state(uint32_t device_id) const;

  // Faults whose active state at the time of recording equals `state`.
  std::vector<const FaultEvent*> faults_in_state(uint16_t state) const;

  // Counts of recorded faults indexed by Severity (Info..Critical).
  std::array<size_t, 4> fault_count_by_severity() const;

  // Fault bursts per device: runs of at least min_count faults that all fall
  // within window_ns of the run's first fault. Reported windows are maximal
  // and do not overlap. A min_count of 0 is treated as 1.
  std::vector<BurstWindow> burst_windows(uint64_t window_ns,
                                         size_t min_count) const;

  AnomalySummary summary() const;

 private:
  struct DeviceState {
    uint16_t current_state = 0;
    bool has_state = false;
  };

  void handle_transition(uint32_t dev, const Frame& f);
  void handle_config(uint32_t dev, const Frame& f);
  void handle_fault(uint32_t dev, const Frame& f);
  void diag(AnomalyDiagCode c, uint32_t dev, uint32_t seq, std::string detail);

  std::vector<FaultEvent> faults_;
  std::vector<TransitionEvent> transitions_;
  std::vector<ConfigEvent> configs_;
  std::vector<AnomalyDiagnostic> diags_;
  std::map<uint32_t, DeviceState> devices_;
};

}  // namespace blackspan

#endif  // BLACKSPAN_ANOMALY_H
