#include "blackspan/anomaly.h"

#include <algorithm>
#include <cstdio>
#include <set>
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

std::string dev_hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08x", v);
  return std::string(b);
}

std::string byte_hex(uint8_t v) {
  char b[8];
  std::snprintf(b, sizeof(b), "0x%02x", static_cast<unsigned>(v));
  return std::string(b);
}

}  // namespace

const char* anomaly_diag_str(AnomalyDiagCode c) {
  switch (c) {
    case AnomalyDiagCode::StateDesync:
      return "state-desync";
    case AnomalyDiagCode::TruncatedPayload:
      return "truncated-payload";
    case AnomalyDiagCode::UnknownRecord:
      return "unknown-record";
  }
  return "unknown";
}

void AnomalyEngine::diag(AnomalyDiagCode c, uint32_t dev, uint32_t seq,
                         std::string detail) {
  AnomalyDiagnostic d;
  d.code = c;
  d.device_id = dev;
  d.session_seq = seq;
  d.detail = std::move(detail);
  diags_.push_back(std::move(d));
}

void AnomalyEngine::handle_transition(uint32_t dev, const Frame& f) {
  StateTransition rec;
  const DecodeStatus st =
      decode_state_transition(f.payload.data(), f.payload.size(), rec);

  DeviceState& ds = devices_[dev];
  bool continues = true;
  if (ds.has_state && rec.from_state != ds.current_state) {
    continues = false;
    diag(AnomalyDiagCode::StateDesync, dev, f.header.session_seq,
         "transition from_state " + u32_dec(rec.from_state) +
             " does not continue last state " + u32_dec(ds.current_state));
  }

  TransitionEvent ev;
  ev.from_state = rec.from_state;
  ev.to_state = rec.to_state;
  ev.cause = rec.cause;
  ev.reserved = rec.reserved;
  ev.device_id = dev;
  ev.session_seq = f.header.session_seq;
  ev.monotonic_ts = f.header.monotonic_ts;
  ev.continues_chain = continues;
  transitions_.push_back(ev);

  // The new state takes effect whether or not the chain was continuous.
  ds.current_state = rec.to_state;
  ds.has_state = true;

  if (st != DecodeStatus::Ok) {
    diag(AnomalyDiagCode::TruncatedPayload, dev, f.header.session_seq,
         "state-transition payload truncated");
  }
}

void AnomalyEngine::handle_config(uint32_t dev, const Frame& f) {
  ConfigChange rec;
  const DecodeStatus st =
      decode_config_change(f.payload.data(), f.payload.size(), rec);

  ConfigEvent ev;
  ev.param_id = rec.param_id;
  ev.old_value = rec.old_value;
  ev.new_value = rec.new_value;
  ev.device_id = dev;
  ev.session_seq = f.header.session_seq;
  ev.monotonic_ts = f.header.monotonic_ts;
  configs_.push_back(ev);

  if (st != DecodeStatus::Ok) {
    diag(AnomalyDiagCode::TruncatedPayload, dev, f.header.session_seq,
         "config-change payload truncated");
  }
}

void AnomalyEngine::handle_fault(uint32_t dev, const Frame& f) {
  Fault rec;
  const DecodeStatus st = decode_fault(f.payload.data(), f.payload.size(), rec);

  FaultEvent ev;
  ev.fault_code = rec.fault_code;
  ev.severity = rec.severity;
  ev.subsystem_id = rec.subsystem_id;
  ev.detail = std::move(rec.detail);
  ev.device_id = dev;
  ev.session_seq = f.header.session_seq;
  ev.monotonic_ts = f.header.monotonic_ts;

  const auto it = devices_.find(dev);
  if (it != devices_.end() && it->second.has_state) {
    ev.state_at_fault = it->second.current_state;
  }
  faults_.push_back(std::move(ev));

  if (st != DecodeStatus::Ok) {
    diag(AnomalyDiagCode::TruncatedPayload, dev, f.header.session_seq,
         "fault payload truncated");
  }
}

void AnomalyEngine::accept(const Frame& f) {
  const RecordType t = f.header.type;
  const uint32_t dev = f.header.device_id;

  if (!is_known_record_type(t)) {
    diag(AnomalyDiagCode::UnknownRecord, dev, f.header.session_seq,
         "unrecognized record type " + byte_hex(static_cast<uint8_t>(t)));
    return;
  }

  switch (t) {
    case RecordType::StateTransition:
      handle_transition(dev, f);
      break;
    case RecordType::ConfigChange:
      handle_config(dev, f);
      break;
    case RecordType::Fault:
      handle_fault(dev, f);
      break;
    default:
      // A valid record owned by another subsystem; not correlated here.
      break;
  }
}

void AnomalyEngine::ingest(const uint8_t* data, size_t len) {
  FrameStream fs(data, len);
  Frame f;
  while (fs.next(f)) accept(f);
}

std::optional<uint16_t> AnomalyEngine::current_state(uint32_t device_id) const {
  const auto it = devices_.find(device_id);
  if (it == devices_.end() || !it->second.has_state) return std::nullopt;
  return it->second.current_state;
}

std::vector<const FaultEvent*> AnomalyEngine::faults_in_state(
    uint16_t state) const {
  std::vector<const FaultEvent*> out;
  for (const FaultEvent& f : faults_) {
    if (f.state_at_fault.has_value() && f.state_at_fault.value() == state) {
      out.push_back(&f);
    }
  }
  return out;
}

std::array<size_t, 4> AnomalyEngine::fault_count_by_severity() const {
  std::array<size_t, 4> counts{};
  for (const FaultEvent& f : faults_) {
    const size_t idx = static_cast<size_t>(f.severity);
    if (idx < counts.size()) ++counts[idx];
  }
  return counts;
}

std::vector<BurstWindow> AnomalyEngine::burst_windows(uint64_t window_ns,
                                                      size_t min_count) const {
  if (min_count == 0) min_count = 1;

  // Group fault indices by device, preserving arrival order within a device.
  std::map<uint32_t, std::vector<size_t>> by_device;
  for (size_t i = 0; i < faults_.size(); ++i) {
    by_device[faults_[i].device_id].push_back(i);
  }

  std::vector<BurstWindow> out;
  for (auto& kv : by_device) {
    std::vector<size_t>& idx = kv.second;
    // Order this device's faults by timestamp; ties keep arrival order so the
    // scan below is deterministic regardless of interleaving.
    std::stable_sort(idx.begin(), idx.end(), [this](size_t a, size_t b) {
      return faults_[a].monotonic_ts < faults_[b].monotonic_ts;
    });

    const size_t n = idx.size();
    size_t i = 0;
    while (i < n) {
      const uint64_t start_ts = faults_[idx[i]].monotonic_ts;
      size_t j = i;
      while (j + 1 < n &&
             faults_[idx[j + 1]].monotonic_ts - start_ts <= window_ns) {
        ++j;
      }
      const size_t count = j - i + 1;
      if (count >= min_count) {
        BurstWindow w;
        w.device_id = kv.first;
        w.count = count;
        w.first_ts = start_ts;
        w.last_ts = faults_[idx[j]].monotonic_ts;
        w.first_seq = faults_[idx[i]].session_seq;
        w.last_seq = faults_[idx[j]].session_seq;
        w.member_indices.reserve(count);
        for (size_t k = i; k <= j; ++k) w.member_indices.push_back(idx[k]);
        out.push_back(std::move(w));
        i = j + 1;  // windows do not overlap
      } else {
        ++i;
      }
    }
  }
  return out;
}

AnomalySummary AnomalyEngine::summary() const {
  AnomalySummary out;
  out.total_faults = faults_.size();
  out.total_transitions = transitions_.size();
  out.total_config_changes = configs_.size();
  out.faults_by_severity = fault_count_by_severity();
  out.diagnostic_count = diags_.size();

  std::set<uint32_t> devs;
  for (const FaultEvent& e : faults_) devs.insert(e.device_id);
  for (const TransitionEvent& e : transitions_) devs.insert(e.device_id);
  for (const ConfigEvent& e : configs_) devs.insert(e.device_id);
  out.device_count = devs.size();

  for (const AnomalyDiagnostic& d : diags_) {
    if (d.code == AnomalyDiagCode::StateDesync) ++out.state_desyncs;
  }
  return out;
}

std::string describe_fault(const FaultEvent& f) {
  std::string out = "device " + dev_hex(f.device_id) + " fault code " +
                    u32_dec(f.fault_code) + " severity " +
                    severity_name(f.severity) + " subsystem " +
                    u32_dec(f.subsystem_id) + " seq " + u32_dec(f.session_seq) +
                    " ts " + u64_dec(f.monotonic_ts);
  if (f.state_at_fault.has_value()) {
    out += " in state " + u32_dec(f.state_at_fault.value());
  } else {
    out += " in state unknown";
  }
  out += " (" + std::to_string(f.detail.size()) + " detail bytes)";
  return out;
}

std::string describe_transition(const TransitionEvent& t) {
  std::string out = "device " + dev_hex(t.device_id) + " transition " +
                    u32_dec(t.from_state) + " -> " + u32_dec(t.to_state) +
                    " cause " + u32_dec(t.cause) + " seq " +
                    u32_dec(t.session_seq) + " ts " + u64_dec(t.monotonic_ts);
  if (!t.continues_chain) out += " (desync)";
  return out;
}

std::string describe_config(const ConfigEvent& c) {
  return "device " + dev_hex(c.device_id) + " config param " +
         u32_dec(c.param_id) + " " + u32_dec(c.old_value) + " -> " +
         u32_dec(c.new_value) + " seq " + u32_dec(c.session_seq) + " ts " +
         u64_dec(c.monotonic_ts);
}

std::string describe_anomaly_summary(const AnomalySummary& s) {
  std::string out;
  out += std::to_string(s.total_faults) + " faults, " +
         std::to_string(s.total_transitions) + " transitions, " +
         std::to_string(s.total_config_changes) + " config changes across " +
         std::to_string(s.device_count) + " devices; severity " +
         std::to_string(s.faults_by_severity[0]) + "/" +
         std::to_string(s.faults_by_severity[1]) + "/" +
         std::to_string(s.faults_by_severity[2]) + "/" +
         std::to_string(s.faults_by_severity[3]) +
         " (info/warning/error/critical); " +
         std::to_string(s.state_desyncs) + " state desyncs; " +
         std::to_string(s.diagnostic_count) + " diagnostics";
  return out;
}

}  // namespace blackspan
