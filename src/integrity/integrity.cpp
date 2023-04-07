#include "blackspan/integrity.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

#include "blackspan/channel.h"
#include "blackspan/frame.h"
#include "blackspan/schema.h"
#include "blackspan/session.h"

namespace blackspan {
namespace {

constexpr size_t kDeviceCountSoftLimit = 256;

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

IntegrityLevel level_of(IntegrityCode c) {
  switch (c) {
    case IntegrityCode::FrameCrcSuspect:
    case IntegrityCode::TruncatedStream:
    case IntegrityCode::SessionEndWithoutBegin:
    case IntegrityCode::SampleWithoutSchema:
      return IntegrityLevel::Error;

    case IntegrityCode::NonMonotonicSeq:
    case IntegrityCode::NonMonotonicTs:
    case IntegrityCode::UnknownRecordType:
    case IntegrityCode::SessionUnterminated:
    case IntegrityCode::SchemaRedefined:
    case IntegrityCode::DeviceCountAnomaly:
      return IntegrityLevel::Warning;

    case IntegrityCode::Ok:
    case IntegrityCode::EmptyRecording:
      return IntegrityLevel::Info;
  }
  return IntegrityLevel::Info;
}

std::string dec(uint64_t v) {
  return std::to_string(static_cast<unsigned long long>(v));
}

std::string byte_hex(uint8_t v) {
  char b[8];
  std::snprintf(b, sizeof(b), "0x%02x", static_cast<unsigned>(v));
  return std::string(b);
}

void add(std::vector<IntegrityFinding>& out, IntegrityCode code, uint32_t dev,
         uint32_t seq, uint64_t ts, std::string detail) {
  IntegrityFinding f;
  f.code = code;
  f.level = level_of(code);
  f.device_id = dev;
  f.session_seq = seq;
  f.ts = ts;
  f.detail = std::move(detail);
  out.push_back(std::move(f));
}

std::optional<uint16_t> payload_channel(const std::vector<uint8_t>& p) {
  if (p.size() < 2) return std::nullopt;
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                               (static_cast<uint16_t>(p[1]) << 8));
}

std::optional<uint16_t> sample_block_channel(const std::vector<uint8_t>& p) {
  SampleBlockHeader h;
  if (decode_sample_block_header(p.data(), p.size(), h) == ChannelError::None) {
    return h.channel_id;
  }
  return payload_channel(p);
}

std::string schema_field_note(const std::vector<uint8_t>& p) {
  SchemaDecodeResult sr = decode_channel_schema(p.data(), p.size());
  if (!sr.ok()) return std::string();
  return " (" + std::to_string(sr.schema.fields.size()) + " field(s))";
}

struct DeviceScan {
  bool have_last = false;
  uint32_t last_seq = 0;
  uint64_t last_ts = 0;
  std::vector<std::pair<uint32_t, uint64_t>> open_begins;
};

void append_terminal_status(FrameStatus s, size_t frames_total,
                            std::vector<IntegrityFinding>& out) {
  switch (s) {
    case FrameStatus::Ok:
      return;
    case FrameStatus::Incomplete:
      add(out, IntegrityCode::TruncatedStream, 0, 0, 0,
          "stream ends mid-frame after " + std::to_string(frames_total) +
              " complete frame(s)");
      return;
    case FrameStatus::HeaderCrcMismatch:
    case FrameStatus::PayloadCrcMismatch:
      add(out, IntegrityCode::FrameCrcSuspect, 0, 0, 0,
          std::string("frame crc check failed: ") + frame_status_str(s));
      return;
    case FrameStatus::BadMagic:
    case FrameStatus::BadVersion:
    case FrameStatus::PayloadTooLong:
      add(out, IntegrityCode::TruncatedStream, 0, 0, 0,
          std::string("decode halted: ") + frame_status_str(s));
      return;
  }
}

void scan_frames(const uint8_t* data, size_t len, IntegrityReport& r) {
  std::vector<IntegrityFinding>& out = r.findings;

  std::map<uint32_t, DeviceScan> devices;
  std::set<uint16_t> schema_channels;
  size_t frames_total = 0;
  size_t frames_ok = 0;

  FrameStream fs(data, len);
  Frame f;
  while (fs.next(f)) {
    ++frames_total;
    const uint32_t dev = f.header.device_id;
    const uint32_t seq = f.header.session_seq;
    const uint64_t ts = f.header.monotonic_ts;
    DeviceScan& d = devices[dev];

    const bool known = is_known_record_type(f.header.type);
    if (known) ++frames_ok;

    if (d.have_last) {
      if (seq < d.last_seq) {
        add(out, IntegrityCode::NonMonotonicSeq, dev, seq, ts,
            "session_seq " + dec(seq) + " < previous " + dec(d.last_seq));
      }
      if (ts < d.last_ts) {
        add(out, IntegrityCode::NonMonotonicTs, dev, seq, ts,
            "monotonic_ts " + dec(ts) + " < previous " + dec(d.last_ts));
      }
    }
    d.last_seq = seq;
    d.last_ts = ts;
    d.have_last = true;

    if (!known) {
      add(out, IntegrityCode::UnknownRecordType, dev, seq, ts,
          "record type " + byte_hex(static_cast<uint8_t>(f.header.type)) +
              " is not defined");
      continue;
    }

    switch (f.header.type) {
      case RecordType::SessionBegin:
        d.open_begins.emplace_back(seq, ts);
        break;
      case RecordType::SessionEnd:
        if (d.open_begins.empty()) {
          add(out, IntegrityCode::SessionEndWithoutBegin, dev, seq, ts,
              "session-end with no active session-begin");
        } else {
          d.open_begins.pop_back();
        }
        break;
      case RecordType::SchemaDef: {
        std::optional<uint16_t> ch = payload_channel(f.payload);
        if (ch) {
          if (!schema_channels.insert(*ch).second) {
            add(out, IntegrityCode::SchemaRedefined, dev, seq, ts,
                "channel " + dec(*ch) + " already had a schema-def" +
                    schema_field_note(f.payload));
          }
        }
        break;
      }
      case RecordType::SampleBlock: {
        std::optional<uint16_t> ch = sample_block_channel(f.payload);
        if (ch && schema_channels.find(*ch) == schema_channels.end()) {
          add(out, IntegrityCode::SampleWithoutSchema, dev, seq, ts,
              "sample block for channel " + dec(*ch) +
                  " precedes any schema-def");
        }
        break;
      }
      default:
        break;
    }
  }

  for (auto& kv : devices) {
    for (const auto& ob : kv.second.open_begins) {
      add(out, IntegrityCode::SessionUnterminated, kv.first, ob.first, ob.second,
          "session-begin left open at end of stream");
    }
  }

  if (devices.size() > kDeviceCountSoftLimit) {
    add(out, IntegrityCode::DeviceCountAnomaly, 0, 0, 0,
        dec(devices.size()) + " distinct devices exceeds soft limit " +
            dec(kDeviceCountSoftLimit));
  }

  if (frames_total == 0) {
    add(out, IntegrityCode::EmptyRecording, 0, 0, 0,
        "no complete frames decoded");
  }

  append_terminal_status(fs.status(), frames_total, out);

  r.frames_total = frames_total;
  r.frames_ok = frames_ok;
}

using FindingKey = std::tuple<uint8_t, uint32_t, uint32_t>;

FindingKey key_of(IntegrityCode c, uint32_t dev, uint32_t seq) {
  return FindingKey(static_cast<uint8_t>(c), dev, seq);
}

void cross_check_model(const DecodedRecording& rec, IntegrityReport& r) {
  std::vector<IntegrityFinding>& out = r.findings;

  std::set<FindingKey> seen;
  for (const IntegrityFinding& f : out) {
    seen.insert(key_of(f.code, f.device_id, f.session_seq));
  }

  auto note = [&](IntegrityCode c, uint32_t dev, uint32_t seq, uint64_t ts,
                  std::string detail) {
    FindingKey k = key_of(c, dev, seq);
    if (seen.find(k) != seen.end()) return;
    seen.insert(k);
    add(out, c, dev, seq, ts, std::move(detail));
  };

  for (const Session& s : rec.sessions().sessions()) {
    if (!s.closed) {
      note(IntegrityCode::SessionUnterminated, s.device_id, s.begin_seq,
           s.begin_ts,
           "reconstructed session #" + dec(s.session_index) +
               " left open at end of stream");
    }
  }

  for (const SessionDiagnostic& sd : rec.sessions().diagnostics()) {
    switch (sd.code) {
      case SessionDiagCode::EndWithoutBegin:
        note(IntegrityCode::SessionEndWithoutBegin, sd.device_id,
             sd.session_seq, 0, sd.detail);
        break;
      case SessionDiagCode::UnterminatedSession:
        note(IntegrityCode::SessionUnterminated, sd.device_id, sd.session_seq,
             0, sd.detail);
        break;
      case SessionDiagCode::UnknownRecordInSession:
        note(IntegrityCode::UnknownRecordType, sd.device_id, sd.session_seq, 0,
             sd.detail);
        break;
      default:
        break;
    }
  }
}

void finalize(IntegrityReport& r) {
  std::stable_sort(r.findings.begin(), r.findings.end(),
                   [](const IntegrityFinding& a, const IntegrityFinding& b) {
                     if (a.ts != b.ts) return a.ts < b.ts;
                     return static_cast<uint8_t>(a.code) <
                            static_cast<uint8_t>(b.code);
                   });
  r.error_count = 0;
  r.warning_count = 0;
  for (const IntegrityFinding& f : r.findings) {
    if (f.level == IntegrityLevel::Error) {
      ++r.error_count;
    } else if (f.level == IntegrityLevel::Warning) {
      ++r.warning_count;
    }
  }
}

}

bool IntegrityReport::clean() const {
  return error_count == 0 && warning_count == 0;
}

const char* integrity_level_name(IntegrityLevel l) {
  switch (l) {
    case IntegrityLevel::Info: return "info";
    case IntegrityLevel::Warning: return "warning";
    case IntegrityLevel::Error: return "error";
  }
  return "unknown";
}

const char* integrity_code_name(IntegrityCode c) {
  switch (c) {
    case IntegrityCode::Ok: return "ok";
    case IntegrityCode::FrameCrcSuspect: return "frame-crc-suspect";
    case IntegrityCode::TruncatedStream: return "truncated-stream";
    case IntegrityCode::NonMonotonicSeq: return "non-monotonic-seq";
    case IntegrityCode::NonMonotonicTs: return "non-monotonic-ts";
    case IntegrityCode::UnknownRecordType: return "unknown-record-type";
    case IntegrityCode::SessionUnterminated: return "session-unterminated";
    case IntegrityCode::SessionEndWithoutBegin:
      return "session-end-without-begin";
    case IntegrityCode::SchemaRedefined: return "schema-redefined";
    case IntegrityCode::SampleWithoutSchema: return "sample-without-schema";
    case IntegrityCode::EmptyRecording: return "empty-recording";
    case IntegrityCode::DeviceCountAnomaly: return "device-count-anomaly";
  }
  return "unknown";
}

std::string describe_integrity_finding(const IntegrityFinding& f) {
  char head[128];
  std::snprintf(head, sizeof(head), "%-7s [%s] dev=%08x seq=%u ts=%llu: ",
                integrity_level_name(f.level), integrity_code_name(f.code),
                f.device_id, f.session_seq,
                static_cast<unsigned long long>(f.ts));
  std::string s = head;
  s += f.detail;
  return s;
}

IntegrityReport audit_frames(const uint8_t* data, size_t len) {
  IntegrityReport r;
  scan_frames(data, len, r);
  finalize(r);
  return r;
}

IntegrityReport audit_recording(const DecodedRecording& rec,
                                const uint8_t* data, size_t len) {
  IntegrityReport r;
  scan_frames(data, len, r);
  cross_check_model(rec, r);
  finalize(r);
  return r;
}

std::string render_integrity(const IntegrityReport& r) {
  std::string out;
  char buf[96];
  std::snprintf(buf, sizeof(buf), "frames: %zu total, %zu ok\n", r.frames_total,
                r.frames_ok);
  out += buf;

  if (r.findings.empty()) {
    out += "ok: no integrity findings\n";
    return out;
  }

  for (const IntegrityFinding& f : r.findings) {
    out += describe_integrity_finding(f);
    out += "\n";
  }
  std::snprintf(buf, sizeof(buf), "%zu error(s), %zu warning(s)\n",
                r.error_count, r.warning_count);
  out += buf;
  return out;
}

}
