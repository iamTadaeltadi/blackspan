#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/derive.h"
#include "blackspan/frame.h"
#include "blackspan/pipeline.h"
#include "blackspan/report.h"
#include "blackspan/schema.h"

using namespace blackspan;

static int failures = 0;
#define CHECK(cond)                                             \
  do {                                                          \
    if (!(cond)) {                                              \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, \
                   __FILE__, __LINE__);                         \
      ++failures;                                               \
    }                                                           \
  } while (0)

static void test_lex_blank_and_comment() {
  ScriptLine l;
  AssembleError err = AssembleError::None;
  CHECK(!lex_script_line("", l, &err));
  CHECK(!lex_script_line("   \t ", l, &err));
  CHECK(!lex_script_line("# a comment", l, &err));
  CHECK(!lex_script_line("    # indented comment", l, &err));
  CHECK(err == AssembleError::None);
}

static void test_lex_directive_forms() {
  {
    ScriptLine l;
    AssembleError err = AssembleError::None;
    CHECK(lex_script_line("device 0x1001", l, &err));
    CHECK(err == AssembleError::None);
    CHECK(l.name == "device");
    CHECK(l.args.size() == 1);
    CHECK(l.args[0].key.empty());
    CHECK(l.args[0].value == "0x1001");
  }
  {
    ScriptLine l;
    AssembleError err = AssembleError::None;
    CHECK(lex_script_line("fault code=0x21 severity=error", l, &err));
    CHECK(l.name == "fault");
    CHECK(l.args.size() == 2);
    CHECK(find_arg(l, "code") != nullptr);
    CHECK(find_arg(l, "code")->value == "0x21");
    CHECK(find_arg(l, "severity")->value == "error");
    CHECK(find_arg(l, "missing") == nullptr);
  }
  {

    ScriptLine l;
    AssembleError err = AssembleError::None;
    CHECK(lex_script_line("marker text=\"a b  c\" kind=2  # note", l, &err));
    CHECK(err == AssembleError::None);
    CHECK(l.args.size() == 2);
    CHECK(find_arg(l, "text")->value == "a b  c");
    CHECK(find_arg(l, "text")->quoted);
    CHECK(find_arg(l, "kind")->value == "2");
  }
  {

    ScriptLine l;
    AssembleError err = AssembleError::None;
    CHECK(lex_script_line("marker text=\"\"", l, &err));
    CHECK(find_arg(l, "text") != nullptr);
    CHECK(find_arg(l, "text")->value.empty());
    CHECK(find_arg(l, "text")->quoted);
  }
  {
    ScriptLine l;
    AssembleError err = AssembleError::None;
    CHECK(lex_script_line("marker text=\"tab\\there\\nnew\"", l, &err));
    CHECK(err == AssembleError::None);
    CHECK(find_arg(l, "text")->value == "tab\there\nnew");
  }
  {
    ScriptLine l;
    AssembleError err = AssembleError::None;
    CHECK(lex_script_line("marker text=\"hex\\x41\"", l, &err));
    CHECK(find_arg(l, "text")->value == "hexA");
  }
}

static void test_lex_errors() {
  {
    ScriptLine l;
    AssembleError err = AssembleError::None;
    CHECK(lex_script_line("marker text=\"unterminated", l, &err));
    CHECK(err == AssembleError::UnterminatedString);
  }
  {
    ScriptLine l;
    AssembleError err = AssembleError::None;
    CHECK(lex_script_line("marker text=\"bad\\q\"", l, &err));
    CHECK(err == AssembleError::BadEscape);
  }
}

static void test_parse_uint() {
  uint64_t v = 0;
  CHECK(parse_uint("0", UINT64_MAX, v) && v == 0);
  CHECK(parse_uint("42", UINT64_MAX, v) && v == 42);
  CHECK(parse_uint("0x1f", UINT64_MAX, v) && v == 31);
  CHECK(parse_uint("0X1F", UINT64_MAX, v) && v == 31);
  CHECK(parse_uint("0b1011", UINT64_MAX, v) && v == 11);
  CHECK(parse_uint("1_000", UINT64_MAX, v) && v == 1000);
  CHECK(parse_uint("18446744073709551615", UINT64_MAX, v) && v == UINT64_MAX);

  CHECK(!parse_uint("", UINT64_MAX, v));
  CHECK(!parse_uint("-1", UINT64_MAX, v));
  CHECK(!parse_uint("12a", UINT64_MAX, v));
  CHECK(!parse_uint("0x", UINT64_MAX, v));
  CHECK(!parse_uint("0b2", UINT64_MAX, v));
  CHECK(!parse_uint("18446744073709551616", UINT64_MAX, v));
  CHECK(!parse_uint("256", UINT8_MAX, v));
  CHECK(parse_uint("255", UINT8_MAX, v) && v == 255);
}

static void test_parse_int() {
  int64_t v = 0;
  CHECK(parse_int("0", -10, 10, v) && v == 0);
  CHECK(parse_int("-7", -10, 10, v) && v == -7);
  CHECK(parse_int("-0x10", INT32_MIN, INT32_MAX, v) && v == -16);
  CHECK(parse_int("-128", INT8_MIN, INT8_MAX, v) && v == -128);
  CHECK(!parse_int("-129", INT8_MIN, INT8_MAX, v));
  CHECK(!parse_int("128", INT8_MIN, INT8_MAX, v));
  CHECK(parse_int("-9223372036854775808", INT64_MIN, INT64_MAX, v) &&
        v == INT64_MIN);
  CHECK(!parse_int("", -10, 10, v));
  CHECK(!parse_int("-", -10, 10, v));
}

static void test_parse_real() {
  double d = 0.0;
  CHECK(parse_real("1.5", d) && d == 1.5);
  CHECK(parse_real("-0.25", d) && d == -0.25);
  CHECK(parse_real("1e3", d) && d == 1000.0);
  CHECK(parse_real("nan", d) && std::isnan(d));
  CHECK(parse_real("inf", d) && std::isinf(d) && d > 0);
  CHECK(parse_real("-inf", d) && std::isinf(d) && d < 0);
  CHECK(!parse_real("1.5x", d));
  CHECK(!parse_real("", d));
}

static void test_parse_names() {
  CHECK(parse_field_type_name("u8") == FieldType::U8);
  CHECK(parse_field_type_name("i16") == FieldType::I16);
  CHECK(parse_field_type_name("f64") == FieldType::F64);
  CHECK(!parse_field_type_name("u24").has_value());
  CHECK(!parse_field_type_name("").has_value());

  CHECK(parse_severity_name("info") == 0);
  CHECK(parse_severity_name("warning") == 1);
  CHECK(parse_severity_name("warn") == 1);
  CHECK(parse_severity_name("error") == 2);
  CHECK(parse_severity_name("critical") == 3);
  CHECK(parse_severity_name("3") == 3);
  CHECK(!parse_severity_name("4").has_value());
  CHECK(!parse_severity_name("fatal").has_value());
}

static void test_parse_hex() {
  std::vector<uint8_t> out;
  CHECK(parse_hex_bytes("", out) && out.empty());
  CHECK(parse_hex_bytes("00ff10", out) && out.size() == 3);
  CHECK(out[0] == 0x00 && out[1] == 0xff && out[2] == 0x10);
  CHECK(parse_hex_bytes("AbCd", out) && out[0] == 0xab && out[1] == 0xcd);
  CHECK(!parse_hex_bytes("abc", out));
  CHECK(!parse_hex_bytes("zz", out));
}

static ChannelSchema imu_schema(uint16_t id = 3) {
  ChannelSchema s;
  s.channel_id = id;
  s.name = "imu";
  const char* names[] = {"ax", "seq", "ok"};
  FieldType types[] = {FieldType::F32, FieldType::U32, FieldType::U8};
  for (int i = 0; i < 3; ++i) {
    SchemaField f;
    f.name = names[i];
    f.type = types[i];
    s.fields.push_back(f);
  }
  s.recompute_layout();
  return s;
}

static void test_script_schemas() {
  ScriptSchemas ss;
  ChannelSchema a = imu_schema();
  CHECK(ss.declare(a) == 1);
  CHECK(ss.declare(a) == 1);
  CHECK(ss.latest_version(3) == 1);
  CHECK(ss.channel_count() == 1);

  ChannelSchema b = a;
  SchemaField extra;
  extra.name = "temp";
  extra.type = FieldType::I16;
  b.fields.push_back(extra);
  b.recompute_layout();
  CHECK(ss.declare(b) == 2);
  CHECK(ss.latest_version(3) == 2);
  CHECK(ss.get(3, 1) != nullptr && ss.get(3, 1)->fields.size() == 3);
  CHECK(ss.get(3, 2) != nullptr && ss.get(3, 2)->fields.size() == 4);
  CHECK(ss.get(3, 0) == nullptr);
  CHECK(ss.get(3, 3) == nullptr);
  CHECK(ss.get(99, 1) == nullptr);
  CHECK(ss.latest_version(99) == 0);
}

static void test_encode_sample_row() {
  ChannelSchema s = imu_schema();
  CHECK(s.stride == 9);

  std::vector<uint8_t> out;
  size_t bad = 0;
  CHECK(encode_sample_row(s, {"1.5", "100", "1"}, out, &bad));
  CHECK(out.size() == 9);
  float ax = 0;
  std::memcpy(&ax, out.data(), 4);
  CHECK(ax == 1.5f);
  uint32_t seq = 0;
  std::memcpy(&seq, out.data() + 4, 4);
  CHECK(seq == 100);
  CHECK(out[8] == 1);

  out.clear();
  CHECK(!encode_sample_row(s, {"1.5", "100"}, out, &bad));

  out.clear();
  CHECK(!encode_sample_row(s, {"1.5", "100", "256"}, out, &bad));
  CHECK(bad == 2);
  out.clear();
  CHECK(!encode_sample_row(s, {"x", "100", "1"}, out, &bad));
  CHECK(bad == 0);

  ChannelSchema t;
  t.channel_id = 9;
  t.name = "t";
  SchemaField f;
  f.name = "v";
  f.type = FieldType::I16;
  t.fields.push_back(f);
  t.recompute_layout();
  out.clear();
  CHECK(encode_sample_row(t, {"-2"}, out, &bad));
  CHECK(out.size() == 2 && out[0] == 0xfe && out[1] == 0xff);
  out.clear();
  CHECK(!encode_sample_row(t, {"40000"}, out, &bad));
}

static const char* kFullScript =
    "# a complete single-session recording\n"
    "device 0x1001\n"
    "session 1\n"
    "ts 1000\n"
    "tick 10\n"
    "\n"
    "session-begin class=4 fw=1.2.3 epoch=9 label=\"unit-a\"\n"
    "schema id=3 name=imu fields=ax:f32,seq:u32,ok:u8\n"
    "samples channel=3 delta=5\n"
    "  row 1.5 100 1\n"
    "  row -0.5 101 0\n"
    "end\n"
    "state from=0 to=2 cause=1\n"
    "fault code=0x21 severity=error subsystem=3 detail=\"over temp\"\n"
    "marker kind=1 text=\"tech note\"\n"
    "checkpoint id=5 bytes=4096\n"
    "config param=7 old=1 new=2\n"
    "derived id=1 primary=3 name=accel expr=\"ax * 2\"\n"
    "export format=json sections=0xffff\n"
    "session-end reason=2\n";

static void test_assemble_full_recording() {
  AssembleResult r = assemble_script(kFullScript);
  for (const AssembleDiagnostic& d : r.errors) {
    std::fprintf(stderr, "unexpected: %s\n",
                 describe_assemble_diagnostic(d).c_str());
  }
  CHECK(r.ok());
  CHECK(r.frame_count == 11);
  CHECK(!r.bytes.empty());

  DecodedRecording rec;
  rec.decode(r.bytes.data(), r.bytes.size());
  CHECK(rec.frame_count() == 11);
  CHECK(rec.final_status() == FrameStatus::Ok);

  const std::vector<Session>& sessions = rec.sessions().sessions();
  CHECK(sessions.size() == 1);
  if (sessions.size() == 1) {
    const Session& s = sessions[0];
    CHECK(s.device_id == 0x1001);
    CHECK(s.device_class == 4);
    CHECK(s.fw_major == 1 && s.fw_minor == 2 && s.fw_patch == 3);
    CHECK(s.schema_epoch == 9);
    CHECK(s.label == "unit-a");
    CHECK(s.closed);
    CHECK(s.end_reason == 2);
    CHECK(s.begin_seq == 1);
    CHECK(s.begin_ts == 1000);
    CHECK(s.end_seq == 11);
    CHECK(s.end_ts == 1100);
    CHECK(s.duration_ns() == 100);
    CHECK(s.checkpoints.size() == 1);
    CHECK(s.checkpoints[0].checkpoint_id == 5);
    CHECK(s.checkpoints[0].bytes_since_last == 4096);
    CHECK(s.markers.size() == 1);
    CHECK(s.markers[0].marker_kind == 1);
    CHECK(s.markers[0].text == "tech note");
    CHECK(s.seq_regressions == 0);
    CHECK(s.ts_regressions == 0);
  }
  CHECK(rec.sessions().diagnostics().empty());

  CHECK(rec.schemas().count() == 1);
  const ChannelSchema* cs = rec.schemas().latest(3);
  CHECK(cs != nullptr);
  if (cs) {
    CHECK(cs->name == "imu");
    CHECK(cs->version == 1);
    CHECK(cs->fields.size() == 3);
    CHECK(cs->fields[0].name == "ax" && cs->fields[0].type == FieldType::F32);
    CHECK(cs->fields[2].name == "ok" && cs->fields[2].type == FieldType::U8);
    CHECK(cs->stride == 9);
  }

  const ChannelSeries* series = rec.channels().series(3);
  CHECK(series != nullptr);
  if (series) {
    CHECK(series->total_samples() == 2);
    CHECK(series->block_count() == 1);
    CHECK(series->block_base_ts(0) == 1025);
    auto v0 = series->view(0);
    auto v1 = series->view(1);
    CHECK(v0.has_value() && v1.has_value());
    if (v0 && v1) {
      CHECK(v0->get_f32(0).value_or(0.0f) == 1.5f);
      CHECK(v0->get_u32(1).value_or(0) == 100);
      CHECK(v0->get_u8(2).value_or(9) == 1);
      CHECK(v1->get_f32(0).value_or(0.0f) == -0.5f);
      CHECK(v1->get_u32(1).value_or(0) == 101);
      CHECK(v1->get_u8(2).value_or(9) == 0);
    }
  }

  CHECK(rec.anomalies().transitions().size() == 1);
  CHECK(rec.anomalies().config_changes().size() == 1);
  const std::vector<FaultEvent>& faults = rec.anomalies().faults();
  CHECK(faults.size() == 1);
  if (faults.size() == 1) {
    CHECK(faults[0].fault_code == 0x21);
    CHECK(faults[0].severity == Severity::Error);
    CHECK(faults[0].subsystem_id == 3);
    CHECK(std::string(faults[0].detail.begin(), faults[0].detail.end()) ==
          "over temp");
    CHECK(faults[0].state_at_fault.has_value());
    CHECK(faults[0].state_at_fault.value_or(0) == 2);
  }
  if (rec.anomalies().config_changes().size() == 1) {
    const ConfigEvent& c = rec.anomalies().config_changes()[0];
    CHECK(c.param_id == 7 && c.old_value == 1 && c.new_value == 2);
  }
}

static void test_assemble_derived_and_export() {
  AssembleResult r = assemble_script(kFullScript);
  CHECK(r.ok());

  FrameStream fs(r.bytes.data(), r.bytes.size());
  Frame f;
  bool saw_derived = false;
  bool saw_export = false;
  while (fs.next(f)) {
    if (f.header.type == RecordType::DerivedDef) {
      saw_derived = true;
      DerivedDefRecord d;
      CHECK(decode_derived_def(f.payload.data(), f.payload.size(), d) ==
            DeriveError::None);
      CHECK(d.derived_id == 1);
      CHECK(d.primary_channel == 3);
      CHECK(d.name == "accel");
      CHECK(d.expr == "ax * 2");
    } else if (f.header.type == RecordType::ExportRequest) {
      saw_export = true;
      ExportRequest e;
      CHECK(decode_export_request(f.payload.data(), f.payload.size(), e) ==
            ExportError::None);
      CHECK(e.format == ReportFormat::Json);
      CHECK(e.section_mask == 0xffff);
    }
  }
  CHECK(saw_derived);
  CHECK(saw_export);

  DecodedRecording rec;
  rec.decode(r.bytes.data(), r.bytes.size());
  DeriveEngine engine;
  FrameStream fs2(r.bytes.data(), r.bytes.size());
  while (fs2.next(f)) {
    if (f.header.type == RecordType::DerivedDef) {
      CHECK(engine.ingest_frame(f) == DeriveError::None);
    }
  }
  DerivedSeries out;
  CHECK(engine.evaluate(1, rec.channels(), out) == DeriveError::None);
  CHECK(out.values.size() == 2);
  if (out.values.size() == 2) {
    CHECK(out.values[0] == 3.0);
    CHECK(out.values[1] == -1.0);
  }
}

static void test_assemble_deterministic() {
  AssembleResult a = assemble_script(kFullScript);
  AssembleResult b = assemble_script(kFullScript);
  CHECK(a.ok() && b.ok());
  CHECK(a.bytes == b.bytes);
}

static void test_assemble_multi_device() {
  const char* script =
      "tick 5\n"
      "device 1\n"
      "session 1\n"
      "ts 100\n"
      "session-begin label=a\n"
      "device 2\n"
      "session 1\n"
      "ts 200\n"
      "session-begin label=b\n"
      "session-end reason=1\n"
      "device 1\n"
      "session 2\n"
      "session-end reason=0\n";
  AssembleResult r = assemble_script(script);
  CHECK(r.ok());
  CHECK(r.frame_count == 4);

  DecodedRecording rec;
  rec.decode(r.bytes.data(), r.bytes.size());
  CHECK(rec.sessions().sessions().size() == 2);
  SessionSummary sum = rec.sessions().summary();
  CHECK(sum.device_count == 2);
  CHECK(sum.closed_sessions == 2);
  CHECK(sum.unterminated_sessions == 0);
}

static void test_assemble_header_overrides() {

  const char* script =
      "device 7\n"
      "session 10\n"
      "ts 500\n"
      "marker text=first\n"
      "marker text=second dev=99 seq=42 ts=9000\n"
      "marker text=third\n";
  AssembleResult r = assemble_script(script);
  CHECK(r.ok());

  FrameStream fs(r.bytes.data(), r.bytes.size());
  Frame f;
  std::vector<uint32_t> devs, seqs;
  std::vector<uint64_t> stamps;
  while (fs.next(f)) {
    devs.push_back(f.header.device_id);
    seqs.push_back(f.header.session_seq);
    stamps.push_back(f.header.monotonic_ts);
  }
  CHECK(devs.size() == 3);
  if (devs.size() == 3) {
    CHECK(devs[0] == 7 && devs[1] == 99 && devs[2] == 7);
    CHECK(seqs[0] == 10 && seqs[1] == 42 && seqs[2] == 12);
    CHECK(stamps[0] == 500 && stamps[1] == 9000 && stamps[2] == 500);
  }
}

static void test_assemble_schema_versions() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "schema id=4 name=temp fields=v:u16\n"
      "samples channel=4\n"
      "  row 10\n"
      "end\n"
      "schema id=4 name=temp fields=v:u16,extra:u8\n"
      "samples channel=4\n"
      "  row 20 1\n"
      "end\n"
      "samples channel=4 version=1\n"
      "  row 30\n"
      "end\n";
  AssembleResult r = assemble_script(script);
  CHECK(r.ok());

  DecodedRecording rec;
  rec.decode(r.bytes.data(), r.bytes.size());
  CHECK(rec.schemas().total_versions() == 2);
  CHECK(rec.schemas().versions(4).size() == 2);

  const ChannelSeries* s = rec.channels().series(4);
  CHECK(s != nullptr);
  if (s) {
    CHECK(s->schema_version() == 1);
    CHECK(s->total_samples() == 2);
  }
}

static void test_assemble_raw_and_hex() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "raw type=0x07 hex=2100020300\n";
  AssembleResult r = assemble_script(script);
  CHECK(r.ok());
  CHECK(r.frame_count == 1);

  DecodedRecording rec;
  rec.decode(r.bytes.data(), r.bytes.size());
  CHECK(rec.anomalies().faults().size() == 1);
  if (rec.anomalies().faults().size() == 1) {

    const FaultEvent& f = rec.anomalies().faults()[0];
    CHECK(f.fault_code == 0x21);
    CHECK(f.severity == Severity::Error);
    CHECK(f.subsystem_id == 3);
    CHECK(f.detail.empty());
  }
}

static void test_assemble_damage() {
  struct Case {
    const char* mode;
    FrameStatus expect;
  };
  const Case cases[] = {
      {"header-crc", FrameStatus::HeaderCrcMismatch},
      {"payload-crc", FrameStatus::PayloadCrcMismatch},
      {"magic", FrameStatus::BadMagic},
      {"version", FrameStatus::BadVersion},
      {"length", FrameStatus::Incomplete},
  };
  for (const Case& c : cases) {
    std::string script =
        "device 1\n"
        "session 1\n"
        "marker text=ok\n"
        "marker text=broken\n"
        "damage mode=";
    script += c.mode;
    script += "\n";
    AssembleResult r = assemble_script(script);
    CHECK(r.ok());
    CHECK(r.frame_count == 2);

    DecodedRecording rec;
    rec.decode(r.bytes.data(), r.bytes.size());

    CHECK(rec.frame_count() == 1);
    if (rec.final_status() != c.expect) {
      std::fprintf(stderr, "damage %s: got %s, want %s\n", c.mode,
                   frame_status_str(rec.final_status()),
                   frame_status_str(c.expect));
      ++failures;
    }
  }
}

static void test_assemble_damage_truncate() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "marker text=ok\n"
      "marker text=cut\n"
      "damage mode=truncate bytes=6\n";
  AssembleResult r = assemble_script(script);
  CHECK(r.ok());
  DecodedRecording rec;
  rec.decode(r.bytes.data(), r.bytes.size());
  CHECK(rec.frame_count() == 1);
  CHECK(rec.final_status() == FrameStatus::Incomplete);
}

static bool has_error(const AssembleResult& r, AssembleError e, size_t line) {
  for (const AssembleDiagnostic& d : r.errors) {
    if (d.error == e && d.line == line) return true;
  }
  return false;
}

static void test_assemble_diagnostics() {
  const char* script =
      "device 1\n"
      "bogus-directive\n"
      "marker kind=999\n"
      "fault severity=error\n"
      "state from=1 to=2 cause=1 cause=2\n"
      "marker colour=red\n"
      "schema id=5 fields=v:u24\n"
      "samples channel=77\n";
  AssembleResult r = assemble_script(script);
  CHECK(!r.ok());
  CHECK(r.bytes.empty());
  CHECK(has_error(r, AssembleError::UnknownDirective, 2));
  CHECK(has_error(r, AssembleError::ValueOutOfRange, 3));
  CHECK(has_error(r, AssembleError::MissingArgument, 4));
  CHECK(has_error(r, AssembleError::DuplicateArgument, 5));
  CHECK(has_error(r, AssembleError::UnknownArgument, 6));
  CHECK(has_error(r, AssembleError::UnknownFieldType, 7));
  CHECK(has_error(r, AssembleError::UnknownChannel, 8));
  CHECK(r.errors.size() == 7);

  std::string text = describe_assemble_diagnostic(r.errors[0]);
  CHECK(text == "line 2: unknown-directive (bogus-directive)");
}

static void test_assemble_block_diagnostics() {
  {
    const char* script =
        "schema id=1 name=a fields=v:u8\n"
        "row 1\n";
    AssembleResult r = assemble_script(script);
    CHECK(has_error(r, AssembleError::RowOutsideBlock, 2));
  }
  {
    const char* script =
        "schema id=1 name=a fields=v:u8\n"
        "samples channel=1\n"
        "  row 1\n";
    AssembleResult r = assemble_script(script);
    CHECK(has_error(r, AssembleError::UnterminatedBlock, 2));
  }
  {
    const char* script =
        "schema id=1 name=a fields=v:u8\n"
        "samples channel=1\n"
        "end\n";
    AssembleResult r = assemble_script(script);
    CHECK(has_error(r, AssembleError::EmptyBlock, 3));
  }
  {
    const char* script =
        "schema id=1 name=a fields=v:u8,w:u8\n"
        "samples channel=1\n"
        "  row 1\n"
        "end\n";
    AssembleResult r = assemble_script(script);
    CHECK(has_error(r, AssembleError::RowFieldCount, 3));
  }
  {
    const char* script = "end\n";
    AssembleResult r = assemble_script(script);
    CHECK(has_error(r, AssembleError::StrayEnd, 1));
  }
  {
    const char* script =
        "schema id=1 name=a fields=v:u8\n"
        "samples channel=1\n"
        "  marker text=oops\n"
        "end\n";
    AssembleResult r = assemble_script(script);
    CHECK(has_error(r, AssembleError::DirectiveInsideBlock, 3));
  }
  {
    const char* script =
        "schema id=1 name=a fields=v:u8\n"
        "samples channel=1\n"
        "  samples channel=1\n";
    AssembleResult r = assemble_script(script);
    CHECK(has_error(r, AssembleError::BlockInsideBlock, 3));
  }
  {
    AssembleResult r = assemble_script("damage mode=magic\n");
    CHECK(has_error(r, AssembleError::NothingToDamage, 1));
  }
  {
    AssembleResult r = assemble_script(
        "marker text=x\n"
        "damage mode=explode\n");
    CHECK(has_error(r, AssembleError::BadDamageMode, 2));
  }
}

static void test_assemble_empty_and_crlf() {
  AssembleResult empty = assemble_script("");
  CHECK(empty.ok());
  CHECK(empty.frame_count == 0);
  CHECK(empty.bytes.empty());

  AssembleResult crlf = assemble_script(
      "device 1\r\n"
      "session 1\r\n"
      "marker text=x\r\n");
  CHECK(crlf.ok());
  CHECK(crlf.frame_count == 1);
}

int main() {
  test_lex_blank_and_comment();
  test_lex_directive_forms();
  test_lex_errors();
  test_parse_uint();
  test_parse_int();
  test_parse_real();
  test_parse_names();
  test_parse_hex();
  test_script_schemas();
  test_encode_sample_row();
  test_assemble_full_recording();
  test_assemble_derived_and_export();
  test_assemble_deterministic();
  test_assemble_multi_device();
  test_assemble_header_overrides();
  test_assemble_schema_versions();
  test_assemble_raw_and_hex();
  test_assemble_damage();
  test_assemble_damage_truncate();
  test_assemble_diagnostics();
  test_assemble_block_diagnostics();
  test_assemble_empty_and_crlf();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_assemble: all checks passed\n");
  return 0;
}
