#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/byteio.h"
#include "blackspan/channel.h"
#include "blackspan/frame.h"
#include "blackspan/json_writer.h"
#include "blackspan/report.h"
#include "blackspan/schema.h"
#include "blackspan/session.h"

using namespace blackspan;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond,         \
                   __FILE__, __LINE__);                                 \
      ++failures;                                                       \
    }                                                                   \
  } while (0)

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// ---- recorder-stream builders -------------------------------------------

static void put_frame(std::vector<uint8_t>& wire, RecordType type, uint32_t dev,
                      uint32_t seq, uint64_t ts,
                      const std::vector<uint8_t>& payload) {
  FrameHeader h;
  h.type = type;
  h.device_id = dev;
  h.session_seq = seq;
  h.monotonic_ts = ts;
  encode_frame(h, payload.data(), payload.size(), wire);
}

static void put_begin(std::vector<uint8_t>& wire, uint32_t dev, uint32_t seq,
                      uint64_t ts, uint16_t dclass, uint8_t maj, uint8_t min,
                      uint16_t patch, uint32_t epoch, const std::string& label) {
  ByteWriter w;
  w.u16(dclass);
  w.u8(maj);
  w.u8(min);
  w.u16(patch);
  w.u32(epoch);
  w.u8(static_cast<uint8_t>(label.size()));
  w.bytes(reinterpret_cast<const uint8_t*>(label.data()), label.size());
  put_frame(wire, RecordType::SessionBegin, dev, seq, ts, w.buf());
}

static void put_end(std::vector<uint8_t>& wire, uint32_t dev, uint32_t seq,
                    uint64_t ts, uint16_t reason) {
  ByteWriter w;
  w.u16(reason);
  put_frame(wire, RecordType::SessionEnd, dev, seq, ts, w.buf());
}

static void put_checkpoint(std::vector<uint8_t>& wire, uint32_t dev,
                           uint32_t seq, uint64_t ts, uint32_t id,
                           uint32_t bytes_since) {
  ByteWriter w;
  w.u32(id);
  w.u32(bytes_since);
  put_frame(wire, RecordType::Checkpoint, dev, seq, ts, w.buf());
}

static void put_marker(std::vector<uint8_t>& wire, uint32_t dev, uint32_t seq,
                       uint64_t ts, uint8_t kind, const std::string& text) {
  ByteWriter w;
  w.u8(kind);
  w.u8(static_cast<uint8_t>(text.size()));
  w.bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  put_frame(wire, RecordType::Marker, dev, seq, ts, w.buf());
}

static void put_transition(std::vector<uint8_t>& wire, uint32_t dev,
                           uint32_t seq, uint64_t ts, uint16_t from, uint16_t to,
                           uint8_t cause) {
  ByteWriter w;
  w.u16(from);
  w.u16(to);
  w.u8(cause);
  w.u8(0);
  put_frame(wire, RecordType::StateTransition, dev, seq, ts, w.buf());
}

static void put_config(std::vector<uint8_t>& wire, uint32_t dev, uint32_t seq,
                       uint64_t ts, uint16_t param, uint32_t oldv,
                       uint32_t newv) {
  ByteWriter w;
  w.u16(param);
  w.u32(oldv);
  w.u32(newv);
  put_frame(wire, RecordType::ConfigChange, dev, seq, ts, w.buf());
}

static void put_fault(std::vector<uint8_t>& wire, uint32_t dev, uint32_t seq,
                      uint64_t ts, uint16_t code, uint8_t sev, uint8_t subsys,
                      const std::vector<uint8_t>& detail) {
  ByteWriter w;
  w.u16(code);
  w.u8(sev);
  w.u8(subsys);
  w.u8(static_cast<uint8_t>(detail.size()));
  w.bytes(detail);
  put_frame(wire, RecordType::Fault, dev, seq, ts, w.buf());
}

// ---- channel/schema builders (mirrors tests/test_channel.cpp) ------------

using FieldDef = std::pair<std::string, FieldType>;

static ChannelSchema make_schema(uint16_t channel_id, const std::string& name,
                                 const std::vector<FieldDef>& fields) {
  ChannelSchema s;
  s.channel_id = channel_id;
  s.name = name;
  for (const auto& f : fields) {
    SchemaField sf;
    sf.name = f.first;
    sf.type = f.second;
    s.fields.push_back(sf);
  }
  s.recompute_layout();
  return s;
}

// imu: ax(f32), seq(u32), ok(u8), temp(i16), alt(f64); stride 19.
static ChannelSchema imu_schema() {
  return make_schema(3, "imu",
                     {{"ax", FieldType::F32}, {"seq", FieldType::U32},
                      {"ok", FieldType::U8}, {"temp", FieldType::I16},
                      {"alt", FieldType::F64}});
}

static void write_imu_row(ByteWriter& w, int i) {
  const float ax = static_cast<float>(i) * 1.5f;
  uint32_t axbits;
  std::memcpy(&axbits, &ax, 4);
  w.u32(axbits);
  w.u32(static_cast<uint32_t>(100 + i));
  w.u8(static_cast<uint8_t>(i % 2));
  w.u16(static_cast<uint16_t>(static_cast<int16_t>(-i * 10)));
  const double alt = static_cast<double>(i) * 0.25;
  uint64_t altbits;
  std::memcpy(&altbits, &alt, 8);
  w.u64(altbits);
}

static std::vector<uint8_t> block_payload(uint16_t channel_id, uint16_t version,
                                          uint32_t base_ts_delta,
                                          uint16_t sample_count, uint16_t flags,
                                          const std::vector<uint8_t>& rows) {
  ByteWriter w;
  w.u16(channel_id);
  w.u16(version);
  w.u32(base_ts_delta);
  w.u16(sample_count);
  w.u16(flags);
  w.bytes(rows);
  return w.buf();
}

static Frame block_frame(uint16_t channel_id, uint16_t version,
                         uint16_t sample_count, uint64_t monotonic_ts,
                         const std::vector<uint8_t>& rows) {
  Frame f;
  f.header.type = RecordType::SampleBlock;
  f.header.device_id = 0xABCDu;
  f.header.monotonic_ts = monotonic_ts;
  f.payload = block_payload(channel_id, version, 0, sample_count, 0, rows);
  return f;
}

// ---- a shared non-trivial model ------------------------------------------

// A label carrying the three characters a well-formed renderer must escape: a
// double quote, a backslash, and a newline.
static const std::string kLabel = "a\"b\\c\nd";

// Builds the session/anomaly recorder stream once. Device 0xABCD runs a short
// flight: begin, a good transition, an error fault, a config change, a state
// desync, a critical fault, a marker, a checkpoint, then a clean end.
static std::vector<uint8_t> control_wire() {
  std::vector<uint8_t> wire;
  put_begin(wire, 0xABCD, 0, 1000, 42, 3, 7, 100, 0xDEADBEEF, kLabel);
  put_transition(wire, 0xABCD, 1, 1100, 0, 1, 0);
  put_fault(wire, 0xABCD, 2, 1200, 0x10, 2, 5, {1, 2, 3});  // error, state 1
  put_config(wire, 0xABCD, 3, 1250, 7, 1, 2);
  put_transition(wire, 0xABCD, 4, 1300, 5, 2, 1);           // desync (5 != 1)
  put_fault(wire, 0xABCD, 5, 1400, 0x11, 3, 6, {});         // critical, state 2
  put_marker(wire, 0xABCD, 6, 1450, 1, "mk");
  put_checkpoint(wire, 0xABCD, 7, 1480, 9, 4096);
  put_end(wire, 0xABCD, 8, 2000, 5);
  return wire;
}

// A holder so a ReportModel can borrow stable engine instances.
struct Fixture {
  std::vector<uint8_t> wire;
  SessionReconstructor sessions;
  SchemaRegistry schemas;
  ChannelStore channels;
  AnomalyEngine anomalies;

  Fixture() {
    wire = control_wire();
    sessions.reconstruct(wire.data(), wire.size());
    anomalies.ingest(wire.data(), wire.size());

    schemas.register_schema(imu_schema());
    schemas.register_schema(
        make_schema(5, "power", {{"v", FieldType::U16}, {"i", FieldType::I16}}));

    // imu: three real samples so column stats are meaningful.
    ByteWriter rows;
    for (int i = 0; i < 3; ++i) write_imu_row(rows, i);
    channels.ingest_frame(block_frame(3, 1, 3, 5000, rows.buf()), schemas);

    // power: two samples, then compacted to zero so the channel exists with an
    // empty column (exercises the "no stats" branch of the renderers).
    ByteWriter pw;
    pw.u16(10);
    pw.u16(static_cast<uint16_t>(-1));
    pw.u16(20);
    pw.u16(static_cast<uint16_t>(-2));
    channels.ingest_frame(block_frame(5, 1, 2, 6000, pw.buf()), schemas);
    channels.compact_channel(5, 0);
  }

  ReportModel model() const {
    ReportModel m;
    m.sessions = &sessions.sessions();
    m.schemas = &schemas;
    m.channels = &channels;
    m.anomalies = &anomalies;
    return m;
  }
};

// ---- structural validators -----------------------------------------------

// Verifies balanced, correctly-nested {} and [] outside of JSON strings, with
// backslash escapes honored inside strings. A lightweight well-formedness proxy
// that does not need a full parser.
static bool json_structurally_ok(const std::string& s) {
  std::vector<char> want;
  bool in_str = false;
  bool esc = false;
  for (char c : s) {
    if (in_str) {
      if (esc) {
        esc = false;
      } else if (c == '\\') {
        esc = true;
      } else if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
    } else if (c == '{') {
      want.push_back('}');
    } else if (c == '[') {
      want.push_back(']');
    } else if (c == '}' || c == ']') {
      if (want.empty() || want.back() != c) return false;
      want.pop_back();
    }
  }
  return want.empty() && !in_str && !esc;
}

// Counts { and } that sit outside quoted labels (dot labels are double-quoted,
// with \" for an embedded quote) and reports whether they balance.
static bool dot_braces_balanced(const std::string& s) {
  long depth = 0;
  bool in_str = false;
  bool esc = false;
  for (char c : s) {
    if (in_str) {
      if (esc) {
        esc = false;
      } else if (c == '\\') {
        esc = true;
      } else if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth < 0) return false;
    }
  }
  return depth == 0 && !in_str;
}

// ---- tests ----------------------------------------------------------------

static void test_escape_helpers() {
  std::string in = "a\"b\\c\n\t";
  in.push_back('\x01');
  CHECK(escape_json(in) == "a\\\"b\\\\c\\n\\t\\u0001");

  CHECK(escape_dot("x\"y\\z\n\tw") == "x\\\"y\\\\z\\n w");

  // No-op inputs pass through unchanged.
  CHECK(escape_json("plain") == "plain");
  CHECK(escape_dot("plain") == "plain");
}

static void test_format_number() {
  CHECK(format_number(0.0) == "0");
  CHECK(format_number(1.5) == "1.5");
  CHECK(format_number(3.0) == "3");
  CHECK(format_number(-2.25) == "-2.25");
}

static void test_json_writer_basic() {
  JsonWriter w;
  w.begin_object();
  w.key("s");
  w.string_value("he\"llo");
  w.key("n");
  w.int_value(-7);
  w.key("u");
  w.uint_value(42u);
  w.key("d");
  w.double_value(1.5);
  w.key("b");
  w.bool_value(true);
  w.key("z");
  w.null_value();
  w.key("arr");
  w.begin_array();
  w.uint_value(1u);
  w.uint_value(2u);
  w.end_array();
  w.end_object();
  const std::string out = w.str();
  CHECK(out ==
        "{\"s\":\"he\\\"llo\",\"n\":-7,\"u\":42,\"d\":1.5,\"b\":true,"
        "\"z\":null,\"arr\":[1,2]}");
  CHECK(json_structurally_ok(out));

  // Non-finite doubles degrade to null so the document stays well-formed.
  JsonWriter w2;
  w2.begin_array();
  w2.double_value(std::numeric_limits<double>::infinity());
  w2.end_array();
  CHECK(w2.str() == "[null]");
}

static void test_text_render() {
  Fixture fx;
  const std::string out = render_text(fx.model());

  CHECK(has(out, "blackspan report"));
  // Sessions section: header + the reused describe_session line.
  CHECK(has(out, "Sessions (1)"));
  CHECK(has(out, "device 0000abcd"));
  CHECK(has(out, "session #0"));
  CHECK(has(out, "duration 1000ns"));
  CHECK(has(out, "1 checkpoints"));

  // Schemas section lists both registered channels.
  CHECK(has(out, "Schemas (2)"));

  // Channels section: imu with real stats, power with the empty-column dashes.
  CHECK(has(out, "Channels (2)"));
  CHECK(has(out, "channel 3 v1 \"imu\""));
  CHECK(has(out, "stride 19 samples 3"));
  CHECK(has(out, "field[0] \"ax\" (f32) min 0 max 3 mean 1.5"));
  CHECK(has(out, "channel 5 v1 \"power\""));
  CHECK(has(out, "samples 0"));
  CHECK(has(out, "min - max - mean -"));

  // Anomalies section.
  CHECK(has(out, "Anomalies"));
  CHECK(has(out, "faults by severity: info 0, warning 0, error 1, critical 1"));
  CHECK(has(out, "state transitions (2)"));
  CHECK(has(out, "3 -> 4") == false);  // sanity: no bogus transition text
  CHECK(has(out, "(desync)"));
  CHECK(has(out, "state-desync device 0000abcd"));
}

static void test_json_render() {
  Fixture fx;
  const std::string out = render_json(fx.model());

  CHECK(json_structurally_ok(out));
  CHECK(has(out, "\"sessions\":["));
  CHECK(has(out, "\"schemas\":["));
  CHECK(has(out, "\"channels\":["));
  CHECK(has(out, "\"anomalies\":{"));

  // The label's " \\ and newline are correctly escaped in the JSON string.
  CHECK(has(out, "\"label\":\"a\\\"b\\\\c\\nd\""));

  // Numeric stats round to the shared %.6g form.
  CHECK(has(out, "\"mean\":1.5"));
  CHECK(has(out, "\"min\":0"));
  CHECK(has(out, "\"max\":3"));
  // Empty power column renders explicit nulls.
  CHECK(has(out, "\"min\":null,\"max\":null,\"mean\":null"));

  CHECK(has(out, "\"fault_counts\":{\"info\":0,\"warning\":0,\"error\":1,"
                 "\"critical\":1}"));
  CHECK(has(out, "\"continues_chain\":false"));
  CHECK(has(out, "\"severity\":\"critical\""));
}

static void test_dot_render() {
  Fixture fx;
  const std::string out = render_dot(fx.model());

  CHECK(out.rfind("digraph blackspan {", 0) == 0);
  CHECK(dot_braces_balanced(out));
  CHECK(has(out, "session_0 [shape=box"));
  CHECK(has(out, "channel_3 [shape=ellipse"));
  CHECK(has(out, "channel_5 [shape=ellipse"));
  CHECK(has(out, "state_0 -> state_1"));
  CHECK(has(out, "state_5 -> state_2 [label=\"cause 1\", style=dashed"));
  // The escaped label appears; no raw newline survives into the graph.
  CHECK(has(out, "label=\"dev 0000abcd #0 a\\\"b\\\\c\\nd\""));
}

static void test_determinism() {
  Fixture fx;
  const ReportModel m = fx.model();
  CHECK(render_text(m) == render_text(m));
  CHECK(render_json(m) == render_json(m));
  CHECK(render_dot(m) == render_dot(m));

  // A freshly rebuilt fixture reconstructs byte-identical output.
  Fixture fx2;
  CHECK(render_json(fx2.model()) == render_json(m));
  CHECK(render_text(fx2.model()) == render_text(m));
  CHECK(render_dot(fx2.model()) == render_dot(m));
}

static void test_null_sections() {
  Fixture fx;

  // Only sessions present: other sections omitted, no crash.
  ReportModel only_sessions;
  only_sessions.sessions = &fx.sessions.sessions();
  const std::string t = render_text(only_sessions);
  CHECK(has(t, "Sessions (1)"));
  CHECK(!has(t, "Channels ("));
  CHECK(!has(t, "Anomalies"));
  CHECK(!has(t, "Schemas ("));

  const std::string j = render_json(only_sessions);
  CHECK(json_structurally_ok(j));
  CHECK(has(j, "\"sessions\":["));
  CHECK(!has(j, "\"channels\""));
  CHECK(!has(j, "\"anomalies\""));

  const std::string d = render_dot(only_sessions);
  CHECK(dot_braces_balanced(d));
  CHECK(has(d, "session_0"));
  CHECK(!has(d, "channel_"));
  CHECK(!has(d, "state_"));

  // A fully-empty model renders a valid, empty document in every format.
  ReportModel empty;
  CHECK(render_text(empty) == "blackspan report\n================\n");
  CHECK(render_json(empty) == "{}");
  const std::string ed = render_dot(empty);
  CHECK(ed.rfind("digraph blackspan {", 0) == 0);
  CHECK(dot_braces_balanced(ed));
  CHECK(!has(ed, "session_"));
}

static void test_section_mask_filtering() {
  Fixture fx;
  const ReportModel m = fx.model();

  // Just the channels section.
  const std::string only_ch = render_text(m, kSectionChannels);
  CHECK(has(only_ch, "Channels ("));
  CHECK(!has(only_ch, "Sessions ("));
  CHECK(!has(only_ch, "Anomalies"));

  const std::string j = render_json(m, kSectionSessions);
  CHECK(has(j, "\"sessions\":["));
  CHECK(!has(j, "\"channels\""));

  // No known bits set -> a document with no sections.
  CHECK(render_json(m, 0) == "{}");
  CHECK(render_text(m, 0) == "blackspan report\n================\n");
}

static void test_dispatch_and_export_request() {
  Fixture fx;
  const ReportModel m = fx.model();

  CHECK(render(m, ReportFormat::Text) == render_text(m));
  CHECK(render(m, ReportFormat::Json) == render_json(m));
  CHECK(render(m, ReportFormat::Dot) == render_dot(m));

  CHECK(std::string(report_format_name(ReportFormat::Json)) == "json");

  // format_from_byte.
  CHECK(format_from_byte(0).value() == ReportFormat::Text);
  CHECK(format_from_byte(1).value() == ReportFormat::Json);
  CHECK(format_from_byte(2).value() == ReportFormat::Dot);
  CHECK(!format_from_byte(3).has_value());

  // Round-trip decode of a well-formed payload.
  {
    ByteWriter w;
    w.u8(1);        // json
    w.u16(0x0005);  // sessions | channels
    ExportRequest req;
    CHECK(decode_export_request(w.buf().data(), w.buf().size(), req) ==
          ExportError::None);
    CHECK(req.format == ReportFormat::Json);
    CHECK(req.section_mask == 0x0005);

    // render(model, request) honors the mask.
    const std::string r =
        render(m, req.format, req.section_mask ? req.section_mask : kSectionAll);
    CHECK(render(m, req) == r);
    CHECK(has(render(m, req), "\"sessions\":["));
    CHECK(has(render(m, req), "\"channels\":["));
    CHECK(!has(render(m, req), "\"anomalies\""));
  }

  // A zero mask on the wire means "default": render(model, request) treats it
  // as all sections.
  {
    ByteWriter w;
    w.u8(0);  // text
    w.u16(0);
    ExportRequest req;
    CHECK(decode_export_request(w.buf().data(), w.buf().size(), req) ==
          ExportError::None);
    CHECK(req.section_mask == 0);
    CHECK(render(m, req) == render_text(m, kSectionAll));
  }

  // Truncation, bad format, and trailing bytes are distinct errors.
  {
    ExportRequest req;
    const uint8_t two[2] = {1, 0};
    CHECK(decode_export_request(two, 2, req) == ExportError::Truncated);
    CHECK(decode_export_request(nullptr, 0, req) == ExportError::Truncated);

    const uint8_t bad[3] = {9, 0, 0};
    CHECK(decode_export_request(bad, 3, req) == ExportError::BadFormat);

    const uint8_t extra[4] = {1, 0, 0, 0};
    CHECK(decode_export_request(extra, 4, req) == ExportError::TrailingBytes);
  }

  CHECK(std::string(export_error_str(ExportError::None)) == "ok");
  CHECK(std::string(export_error_str(ExportError::BadFormat)) == "bad-format");
  CHECK(std::string(export_error_str(ExportError::TrailingBytes)) ==
        "trailing-bytes");
}

int main() {
  test_escape_helpers();
  test_format_number();
  test_json_writer_basic();
  test_text_render();
  test_json_render();
  test_dot_render();
  test_determinism();
  test_null_sections();
  test_section_mask_filtering();
  test_dispatch_and_export_request();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all report tests passed\n");
  return 0;
}
