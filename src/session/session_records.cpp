#include "blackspan/byteio.h"
#include "blackspan/session.h"

namespace blackspan {

namespace {

// Reads a length-prefixed byte string whose 1-byte length has already been
// consumed. Copies min(declared, available) bytes into out and reports whether
// the full declared length was present.
PayloadStatus read_lp_string(ByteReader& r, uint8_t declared, std::string& out) {
  const size_t want = static_cast<size_t>(declared);
  if (r.remaining() < want) {
    const size_t have = r.remaining();
    const uint8_t* p = r.take(have);
    if (p) out.assign(reinterpret_cast<const char*>(p), have);
    return PayloadStatus::Truncated;
  }
  const uint8_t* p = r.take(want);
  if (p) out.assign(reinterpret_cast<const char*>(p), want);
  return PayloadStatus::Ok;
}

}  // namespace

const char* payload_status_str(PayloadStatus s) {
  switch (s) {
    case PayloadStatus::Ok:
      return "ok";
    case PayloadStatus::Truncated:
      return "truncated";
  }
  return "unknown";
}

PayloadStatus decode_session_begin(const uint8_t* data, size_t len,
                                   SessionBegin& out) {
  out = SessionBegin{};
  ByteReader r(data, len);
  out.device_class = r.u16();
  out.fw_major = r.u8();
  out.fw_minor = r.u8();
  out.fw_patch = r.u16();
  out.schema_epoch = r.u32();
  const uint8_t label_len = r.u8();
  if (!r.ok()) return PayloadStatus::Truncated;
  return read_lp_string(r, label_len, out.label);
}

PayloadStatus decode_session_end(const uint8_t* data, size_t len,
                                 SessionEnd& out) {
  out = SessionEnd{};
  ByteReader r(data, len);
  out.reason_code = r.u16();
  return r.ok() ? PayloadStatus::Ok : PayloadStatus::Truncated;
}

PayloadStatus decode_checkpoint(const uint8_t* data, size_t len,
                                Checkpoint& out) {
  out = Checkpoint{};
  ByteReader r(data, len);
  out.checkpoint_id = r.u32();
  out.bytes_since_last = r.u32();
  return r.ok() ? PayloadStatus::Ok : PayloadStatus::Truncated;
}

PayloadStatus decode_reconnect(const uint8_t* data, size_t len,
                               Reconnect& out) {
  out = Reconnect{};
  ByteReader r(data, len);
  out.gap_ms = r.u32();
  out.prev_session_seq = r.u32();
  return r.ok() ? PayloadStatus::Ok : PayloadStatus::Truncated;
}

PayloadStatus decode_marker(const uint8_t* data, size_t len, Marker& out) {
  out = Marker{};
  ByteReader r(data, len);
  out.marker_kind = r.u8();
  const uint8_t text_len = r.u8();
  if (!r.ok()) return PayloadStatus::Truncated;
  return read_lp_string(r, text_len, out.text);
}

}  // namespace blackspan
