#include "blackspan/frame.h"

#include "blackspan/byteio.h"
#include "blackspan/crc.h"

namespace blackspan {

namespace {

void write_header_fields(ByteWriter& w, const FrameHeader& h) {
  w.u32(kFrameMagic);
  w.u8(h.version);
  w.u8(static_cast<uint8_t>(h.type));
  w.u16(h.flags);
  w.u32(h.device_id);
  w.u32(h.session_seq);
  w.u64(h.monotonic_ts);
  w.u16(h.payload_len);
}

}  // namespace

size_t encode_frame(const FrameHeader& h_in, const uint8_t* payload,
                    size_t payload_len, std::vector<uint8_t>& out) {
  FrameHeader h = h_in;
  h.payload_len = static_cast<uint16_t>(payload_len);

  ByteWriter w;
  write_header_fields(w, h);
  // Header CRC covers the header bytes preceding the CRC field itself.
  const uint16_t hcrc = crc16_ccitt(w.buf().data(), w.buf().size());
  w.u16(hcrc);
  if (payload_len) w.bytes(payload, payload_len);
  const uint32_t pcrc = crc32(payload, payload_len);
  w.u32(pcrc);

  out.insert(out.end(), w.buf().begin(), w.buf().end());
  return w.size();
}

size_t encode_frame(const Frame& f, std::vector<uint8_t>& out) {
  return encode_frame(f.header, f.payload.data(), f.payload.size(), out);
}

FrameStatus decode_frame(const uint8_t* data, size_t len, Frame& out,
                         size_t* consumed) {
  if (consumed) *consumed = 0;
  if (len < kFrameHeaderSize) return FrameStatus::Incomplete;

  ByteReader r(data, len);
  const uint32_t magic = r.u32();
  if (magic != kFrameMagic) return FrameStatus::BadMagic;
  const uint8_t version = r.u8();
  const uint8_t rtype = r.u8();
  const uint16_t flags = r.u16();
  const uint32_t device_id = r.u32();
  const uint32_t session_seq = r.u32();
  const uint64_t ts = r.u64();
  const uint16_t payload_len = r.u16();
  const uint16_t hcrc = r.u16();

  const uint16_t calc_hcrc = crc16_ccitt(data, kFrameHeaderSize - 2);
  if (calc_hcrc != hcrc) return FrameStatus::HeaderCrcMismatch;
  if (version != kFrameVersion) return FrameStatus::BadVersion;

  const size_t total =
      kFrameHeaderSize + static_cast<size_t>(payload_len) + kFrameTrailerSize;
  if (len < total) return FrameStatus::Incomplete;

  const uint8_t* pl = data + kFrameHeaderSize;
  const uint32_t stored_pcrc =
      static_cast<uint32_t>(pl[payload_len]) |
      (static_cast<uint32_t>(pl[payload_len + 1]) << 8) |
      (static_cast<uint32_t>(pl[payload_len + 2]) << 16) |
      (static_cast<uint32_t>(pl[payload_len + 3]) << 24);
  const uint32_t calc_pcrc = crc32(pl, payload_len);
  if (calc_pcrc != stored_pcrc) return FrameStatus::PayloadCrcMismatch;

  out.header.version = version;
  out.header.type = static_cast<RecordType>(rtype);
  out.header.flags = flags;
  out.header.device_id = device_id;
  out.header.session_seq = session_seq;
  out.header.monotonic_ts = ts;
  out.header.payload_len = payload_len;
  out.payload.assign(pl, pl + payload_len);

  if (consumed) *consumed = total;
  return FrameStatus::Ok;
}

const char* frame_status_str(FrameStatus s) {
  switch (s) {
    case FrameStatus::Ok:
      return "ok";
    case FrameStatus::Incomplete:
      return "incomplete";
    case FrameStatus::BadMagic:
      return "bad-magic";
    case FrameStatus::BadVersion:
      return "bad-version";
    case FrameStatus::HeaderCrcMismatch:
      return "header-crc-mismatch";
    case FrameStatus::PayloadCrcMismatch:
      return "payload-crc-mismatch";
    case FrameStatus::PayloadTooLong:
      return "payload-too-long";
  }
  return "unknown";
}

const char* record_type_name(RecordType t) {
  switch (t) {
    case RecordType::SessionBegin:
      return "session-begin";
    case RecordType::SessionEnd:
      return "session-end";
    case RecordType::SchemaDef:
      return "schema-def";
    case RecordType::SampleBlock:
      return "sample-block";
    case RecordType::StateTransition:
      return "state-transition";
    case RecordType::ConfigChange:
      return "config-change";
    case RecordType::Fault:
      return "fault";
    case RecordType::Marker:
      return "marker";
    case RecordType::Checkpoint:
      return "checkpoint";
    case RecordType::Reconnect:
      return "reconnect";
    case RecordType::DerivedDef:
      return "derived-def";
    case RecordType::ExportRequest:
      return "export-request";
  }
  return "unknown";
}

bool FrameStream::next(Frame& out) {
  if (off_ >= len_) return false;
  size_t consumed = 0;
  const FrameStatus s = decode_frame(data_ + off_, len_ - off_, out, &consumed);
  if (s != FrameStatus::Ok) {
    status_ = s;
    return false;
  }
  off_ += consumed;
  status_ = FrameStatus::Ok;
  return true;
}

}  // namespace blackspan
