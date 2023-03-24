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

namespace blackspan {

enum class DecodeStatus {
  Ok,
  Truncated,
};

const char* decode_status_str(DecodeStatus s);

enum class Severity : uint8_t {
  Info = 0,
  Warning = 1,
  Error = 2,
  Critical = 3,
};

const char* severity_name(Severity s);

Severity severity_from_byte(uint8_t v);

struct StateTransition {
  uint16_t from_state = 0;
  uint16_t to_state = 0;
  uint8_t cause = 0;
  uint8_t reserved = 0;
};

struct ConfigChange {
  uint16_t param_id = 0;
  uint32_t old_value = 0;
  uint32_t new_value = 0;
};

struct Fault {
  uint16_t fault_code = 0;
  Severity severity = Severity::Info;
  uint8_t subsystem_id = 0;
  std::vector<uint8_t> detail;
};

DecodeStatus decode_state_transition(const uint8_t* data, size_t len,
                                     StateTransition& out);
DecodeStatus decode_config_change(const uint8_t* data, size_t len,
                                  ConfigChange& out);
DecodeStatus decode_fault(const uint8_t* data, size_t len, Fault& out);

struct TransitionEvent {
  uint16_t from_state = 0;
  uint16_t to_state = 0;
  uint8_t cause = 0;
  uint8_t reserved = 0;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  uint64_t monotonic_ts = 0;
  bool continues_chain = true;
};

struct ConfigEvent {
  uint16_t param_id = 0;
  uint32_t old_value = 0;
  uint32_t new_value = 0;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  uint64_t monotonic_ts = 0;
};

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
  StateDesync,
  TruncatedPayload,
  UnknownRecord,
};

const char* anomaly_diag_str(AnomalyDiagCode c);

struct AnomalyDiagnostic {
  AnomalyDiagCode code;
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  std::string detail;
};

struct BurstWindow {
  uint32_t device_id = 0;
  size_t count = 0;
  uint64_t first_ts = 0;
  uint64_t last_ts = 0;
  uint32_t first_seq = 0;
  uint32_t last_seq = 0;
  std::vector<size_t> member_indices;
};

struct AnomalySummary {
  size_t device_count = 0;
  size_t total_faults = 0;
  size_t total_transitions = 0;
  size_t total_config_changes = 0;
  size_t state_desyncs = 0;
  std::array<size_t, 4> faults_by_severity{};
  size_t diagnostic_count = 0;
};

std::string describe_fault(const FaultEvent& f);
std::string describe_transition(const TransitionEvent& t);
std::string describe_config(const ConfigEvent& c);
std::string describe_anomaly_summary(const AnomalySummary& s);

class AnomalyEngine {
 public:
  void accept(const Frame& f);

  void ingest(const uint8_t* data, size_t len);

  const std::vector<FaultEvent>& faults() const { return faults_; }
  const std::vector<TransitionEvent>& transitions() const {
    return transitions_;
  }
  const std::vector<ConfigEvent>& config_changes() const { return configs_; }
  const std::vector<AnomalyDiagnostic>& diagnostics() const { return diags_; }

  std::optional<uint16_t> current_state(uint32_t device_id) const;

  std::vector<const FaultEvent*> faults_in_state(uint16_t state) const;

  std::array<size_t, 4> fault_count_by_severity() const;

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

}

#endif
