#include "blackspan/anomaly.h"
#include "blackspan/byteio.h"

namespace blackspan {

const char* decode_status_str(DecodeStatus s) {
  switch (s) {
    case DecodeStatus::Ok:
      return "ok";
    case DecodeStatus::Truncated:
      return "truncated";
  }
  return "unknown";
}

const char* severity_name(Severity s) {
  switch (s) {
    case Severity::Info:
      return "info";
    case Severity::Warning:
      return "warning";
    case Severity::Error:
      return "error";
    case Severity::Critical:
      return "critical";
  }
  return "unknown";
}

Severity severity_from_byte(uint8_t v) {
  if (v >= static_cast<uint8_t>(Severity::Critical)) return Severity::Critical;
  return static_cast<Severity>(v);
}

DecodeStatus decode_state_transition(const uint8_t* data, size_t len,
                                     StateTransition& out) {
  out = StateTransition{};
  ByteReader r(data, len);
  out.from_state = r.u16();
  out.to_state = r.u16();
  out.cause = r.u8();
  out.reserved = r.u8();
  return r.ok() ? DecodeStatus::Ok : DecodeStatus::Truncated;
}

DecodeStatus decode_config_change(const uint8_t* data, size_t len,
                                  ConfigChange& out) {
  out = ConfigChange{};
  ByteReader r(data, len);
  out.param_id = r.u16();
  out.old_value = r.u32();
  out.new_value = r.u32();
  return r.ok() ? DecodeStatus::Ok : DecodeStatus::Truncated;
}

DecodeStatus decode_fault(const uint8_t* data, size_t len, Fault& out) {
  out = Fault{};
  ByteReader r(data, len);
  out.fault_code = r.u16();
  const uint8_t sev = r.u8();
  out.subsystem_id = r.u8();
  const uint8_t detail_len = r.u8();
  out.severity = severity_from_byte(sev);
  if (!r.ok()) return DecodeStatus::Truncated;

  // Copy min(declared, available) detail bytes and report whether the full
  // declared length was present.
  const size_t want = static_cast<size_t>(detail_len);
  if (r.remaining() < want) {
    const size_t have = r.remaining();
    const uint8_t* p = r.take(have);
    if (p) out.detail.assign(p, p + have);
    return DecodeStatus::Truncated;
  }
  const uint8_t* p = r.take(want);
  if (p) out.detail.assign(p, p + want);
  return DecodeStatus::Ok;
}

}  // namespace blackspan
