#include <cstring>
#include <string>
#include <vector>

#include "assemble_internal.h"
#include "blackspan/byteio.h"
#include "blackspan/crc.h"
#include "blackspan/frame.h"

namespace blackspan {
namespace {

// Appends a length-prefixed string with a single-byte length, truncating at 255
// bytes. The wire grammars that carry names all use this shape.
void write_lp8(const std::string& s, ByteWriter& w) {
  size_t n = s.size() > 255 ? 255 : s.size();
  w.u8(static_cast<uint8_t>(n));
  w.bytes(reinterpret_cast<const uint8_t*>(s.data()), n);
}

void flush(ByteWriter& w, std::vector<uint8_t>& out) {
  out.insert(out.end(), w.buf().begin(), w.buf().end());
}

// Writes a value of the given scalar type in little-endian order. The value
// arrives widened: integers in `u` (already masked to the field width by the
// caller's range check) and reals in `d`.
void write_typed(FieldType t, uint64_t u, double d, ByteWriter& w) {
  switch (t) {
    case FieldType::U8:
    case FieldType::I8:
      w.u8(static_cast<uint8_t>(u));
      break;
    case FieldType::U16:
    case FieldType::I16:
      w.u16(static_cast<uint16_t>(u));
      break;
    case FieldType::U32:
    case FieldType::I32:
      w.u32(static_cast<uint32_t>(u));
      break;
    case FieldType::U64:
    case FieldType::I64:
      w.u64(u);
      break;
    case FieldType::F32: {
      float f = static_cast<float>(d);
      uint32_t bits;
      std::memcpy(&bits, &f, sizeof(bits));
      w.u32(bits);
      break;
    }
    case FieldType::F64: {
      uint64_t bits;
      std::memcpy(&bits, &d, sizeof(bits));
      w.u64(bits);
      break;
    }
  }
}

// Inclusive range of a signed field type, used to reject a row value that would
// silently wrap on the wire.
void signed_range(FieldType t, int64_t& lo, int64_t& hi) {
  switch (t) {
    case FieldType::I8: lo = INT8_MIN; hi = INT8_MAX; break;
    case FieldType::I16: lo = INT16_MIN; hi = INT16_MAX; break;
    case FieldType::I32: lo = INT32_MIN; hi = INT32_MAX; break;
    default: lo = INT64_MIN; hi = INT64_MAX; break;
  }
}

// Recomputes the header CRC of the frame starting at frame_start, so that a
// deliberate edit to a header field leaves the frame internally consistent.
void reseal_header(std::vector<uint8_t>& bytes, size_t frame_start) {
  const uint16_t crc =
      crc16_ccitt(bytes.data() + frame_start, kFrameHeaderSize - 2);
  bytes[frame_start + kFrameHeaderSize - 2] = static_cast<uint8_t>(crc & 0xff);
  bytes[frame_start + kFrameHeaderSize - 1] =
      static_cast<uint8_t>((crc >> 8) & 0xff);
}

// Largest value an unsigned field type can hold.
uint64_t unsigned_max(FieldType t) {
  switch (t) {
    case FieldType::U8: return UINT8_MAX;
    case FieldType::U16: return UINT16_MAX;
    case FieldType::U32: return UINT32_MAX;
    default: return UINT64_MAX;
  }
}

}  // namespace

uint16_t ScriptSchemas::declare(const ChannelSchema& s) {
  std::vector<ChannelSchema>& history = channels_[s.channel_id];
  for (const ChannelSchema& known : history) {
    if (known.same_layout(s)) return known.version;
  }
  ChannelSchema stored = s;
  stored.version = static_cast<uint16_t>(history.size() + 1);
  stored.recompute_layout();
  history.push_back(stored);
  return stored.version;
}

const ChannelSchema* ScriptSchemas::get(uint16_t channel_id,
                                        uint16_t version) const {
  auto it = channels_.find(channel_id);
  if (it == channels_.end()) return nullptr;
  if (version == 0 || version > it->second.size()) return nullptr;
  return &it->second[version - 1];
}

uint16_t ScriptSchemas::latest_version(uint16_t channel_id) const {
  auto it = channels_.find(channel_id);
  if (it == channels_.end()) return 0;
  return static_cast<uint16_t>(it->second.size());
}

bool encode_sample_row(const ChannelSchema& schema,
                       const std::vector<std::string>& values,
                       std::vector<uint8_t>& out, size_t* bad_field) {
  if (bad_field) *bad_field = 0;
  if (values.size() != schema.fields.size()) return false;

  ByteWriter w;
  for (size_t i = 0; i < schema.fields.size(); ++i) {
    const SchemaField& f = schema.fields[i];
    const std::string& text = values[i];
    if (field_type_is_float(f.type)) {
      double d = 0.0;
      if (!parse_real(text, d)) {
        if (bad_field) *bad_field = i;
        return false;
      }
      write_typed(f.type, 0, d, w);
    } else if (field_type_is_signed(f.type)) {
      int64_t lo = 0, hi = 0;
      signed_range(f.type, lo, hi);
      int64_t v = 0;
      if (!parse_int(text, lo, hi, v)) {
        if (bad_field) *bad_field = i;
        return false;
      }
      write_typed(f.type, static_cast<uint64_t>(v), 0.0, w);
    } else {
      uint64_t v = 0;
      if (!parse_uint(text, unsigned_max(f.type), v)) {
        if (bad_field) *bad_field = i;
        return false;
      }
      write_typed(f.type, v, 0.0, w);
    }
  }
  flush(w, out);
  return true;
}

void encode_schema_payload(const ChannelSchema& schema, uint16_t version,
                           std::vector<uint8_t>& out) {
  ByteWriter w;
  w.u16(schema.channel_id);
  w.u8(static_cast<uint8_t>(version & 0xff));
  w.u8(static_cast<uint8_t>(schema.fields.size() & 0xff));
  write_lp8(schema.name, w);
  for (const SchemaField& f : schema.fields) {
    w.u8(static_cast<uint8_t>(f.type));
    write_lp8(f.name, w);
  }
  flush(w, out);
}

namespace assemble_detail {

void write_session_begin(const SessionBegin& r, std::vector<uint8_t>& out) {
  ByteWriter w;
  w.u16(r.device_class);
  w.u8(r.fw_major);
  w.u8(r.fw_minor);
  w.u16(r.fw_patch);
  w.u32(r.schema_epoch);
  write_lp8(r.label, w);
  flush(w, out);
}

void write_session_end(const SessionEnd& r, std::vector<uint8_t>& out) {
  ByteWriter w;
  w.u16(r.reason_code);
  flush(w, out);
}

void write_checkpoint(const Checkpoint& r, std::vector<uint8_t>& out) {
  ByteWriter w;
  w.u32(r.checkpoint_id);
  w.u32(r.bytes_since_last);
  flush(w, out);
}

void write_reconnect(const Reconnect& r, std::vector<uint8_t>& out) {
  ByteWriter w;
  w.u32(r.gap_ms);
  w.u32(r.prev_session_seq);
  flush(w, out);
}

void write_marker(const Marker& r, std::vector<uint8_t>& out) {
  ByteWriter w;
  w.u8(r.marker_kind);
  write_lp8(r.text, w);
  flush(w, out);
}

void write_state_transition(const StateTransition& r,
                            std::vector<uint8_t>& out) {
  ByteWriter w;
  w.u16(r.from_state);
  w.u16(r.to_state);
  w.u8(r.cause);
  w.u8(r.reserved);
  flush(w, out);
}

void write_config_change(const ConfigChange& r, std::vector<uint8_t>& out) {
  ByteWriter w;
  w.u16(r.param_id);
  w.u32(r.old_value);
  w.u32(r.new_value);
  flush(w, out);
}

void write_fault(const Fault& r, std::vector<uint8_t>& out) {
  ByteWriter w;
  w.u16(r.fault_code);
  w.u8(static_cast<uint8_t>(r.severity));
  w.u8(r.subsystem_id);
  size_t n = r.detail.size() > 255 ? 255 : r.detail.size();
  w.u8(static_cast<uint8_t>(n));
  w.bytes(r.detail.data(), n);
  flush(w, out);
}

void write_derived_def(const DerivedDefRecord& r, std::vector<uint8_t>& out) {
  ByteWriter w;
  w.u16(r.derived_id);
  w.u16(r.primary_channel);
  write_lp8(r.name, w);
  size_t n = r.expr.size() > kMaxPayload ? kMaxPayload : r.expr.size();
  w.u16(static_cast<uint16_t>(n));
  w.bytes(reinterpret_cast<const uint8_t*>(r.expr.data()), n);
  flush(w, out);
}

void write_export_request(const ExportRequest& r, std::vector<uint8_t>& out) {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(r.format));
  w.u16(r.section_mask);
  flush(w, out);
}

void write_sample_block_header(uint16_t channel_id, uint16_t schema_version,
                               uint32_t base_ts_delta, uint16_t sample_count,
                               uint16_t flags, std::vector<uint8_t>& out) {
  ByteWriter w;
  w.u16(channel_id);
  w.u16(schema_version);
  w.u32(base_ts_delta);
  w.u16(sample_count);
  w.u16(flags);
  flush(w, out);
}

bool parse_damage_mode(const std::string& s, DamageMode& out) {
  if (s == "header-crc") {
    out = DamageMode::HeaderCrc;
    return true;
  }
  if (s == "payload-crc") {
    out = DamageMode::PayloadCrc;
    return true;
  }
  if (s == "magic") {
    out = DamageMode::Magic;
    return true;
  }
  if (s == "version") {
    out = DamageMode::Version;
    return true;
  }
  if (s == "length") {
    out = DamageMode::Length;
    return true;
  }
  if (s == "truncate") {
    out = DamageMode::Truncate;
    return true;
  }
  return false;
}

bool apply_damage(std::vector<uint8_t>& bytes, size_t frame_start,
                  DamageMode mode, uint64_t amount) {
  if (frame_start >= bytes.size()) return false;
  size_t frame_len = bytes.size() - frame_start;
  if (frame_len < kFrameHeaderSize + kFrameTrailerSize) return false;

  switch (mode) {
    case DamageMode::HeaderCrc:
      // The header CRC occupies the last two header bytes; flipping a bit makes
      // the recomputed CRC disagree without changing any decoded field.
      bytes[frame_start + kFrameHeaderSize - 2] ^= 0xff;
      return true;
    case DamageMode::PayloadCrc:
      bytes[bytes.size() - 1] ^= 0xff;
      return true;
    case DamageMode::Magic:
      bytes[frame_start] ^= 0xff;
      return true;
    case DamageMode::Version:
      // Byte 4 follows the 4-byte magic and holds the format version. The
      // header CRC is recomputed so the frame stays internally consistent and
      // the decoder reaches its version check rather than stopping at the CRC.
      bytes[frame_start + 4] = 0xfe;
      reseal_header(bytes, frame_start);
      return true;
    case DamageMode::Length: {
      // Overstate the declared payload length so the frame claims more bytes
      // than the buffer holds. The length field sits at offset 24, and the
      // header CRC is resealed for the same reason as Version.
      uint16_t claimed = static_cast<uint16_t>(kMaxPayload);
      bytes[frame_start + 24] = static_cast<uint8_t>(claimed & 0xff);
      bytes[frame_start + 25] = static_cast<uint8_t>((claimed >> 8) & 0xff);
      reseal_header(bytes, frame_start);
      return true;
    }
    case DamageMode::Truncate: {
      uint64_t n = amount == 0 ? 1 : amount;
      if (n >= frame_len) return false;
      bytes.resize(bytes.size() - static_cast<size_t>(n));
      return true;
    }
  }
  return false;
}

}  // namespace assemble_detail
}  // namespace blackspan
