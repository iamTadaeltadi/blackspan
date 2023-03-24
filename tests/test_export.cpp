#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/byteio.h"
#include "blackspan/channel.h"
#include "blackspan/export.h"
#include "blackspan/frame.h"
#include "blackspan/schema.h"
#include "blackspan/session.h"

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

namespace {

void put_frame(std::vector<uint8_t>& wire, RecordType type, uint32_t device_id,
               uint32_t seq, uint64_t ts, const std::vector<uint8_t>& payload) {
  FrameHeader h;
  h.type = type;
  h.device_id = device_id;
  h.session_seq = seq;
  h.monotonic_ts = ts;
  encode_frame(h, payload.data(), payload.size(), wire);
}

void put_schema(std::vector<uint8_t>& wire, uint16_t channel_id,
                const std::string& cname, const std::string& fname,
                uint8_t field_type, uint32_t seq, uint64_t ts) {
  ByteWriter p;
  p.u16(channel_id);
  p.u8(1);
  p.u8(1);
  p.u8(static_cast<uint8_t>(cname.size()));
  p.bytes(reinterpret_cast<const uint8_t*>(cname.data()), cname.size());
  p.u8(field_type);
  p.u8(static_cast<uint8_t>(fname.size()));
  p.bytes(reinterpret_cast<const uint8_t*>(fname.data()), fname.size());
  put_frame(wire, RecordType::SchemaDef, 0x100, seq, ts, p.buf());
}

void build_channels(SchemaRegistry& schemas, ChannelStore& channels) {
  std::vector<uint8_t> wire;
  put_schema(wire, 1, "temp", "raw", 3 , 0, 900);
  {
    ByteWriter p;
    p.u16(1);
    p.u16(1);
    p.u32(0);
    p.u16(3);
    p.u16(0);
    p.u16(100);
    p.u16(200);
    p.u16(300);
    put_frame(wire, RecordType::SampleBlock, 0x100, 1, 5000, p.buf());
  }
  put_schema(wire, 2, "c2", "a\"b", 3 , 2, 6900);
  {
    ByteWriter p;
    p.u16(2);
    p.u16(1);
    p.u32(0);
    p.u16(1);
    p.u16(0);
    p.u16(42);
    put_frame(wire, RecordType::SampleBlock, 0x100, 3, 7000, p.buf());
  }
  FrameStream fs(wire.data(), wire.size());
  Frame f;
  while (fs.next(f)) {
    if (f.header.type == RecordType::SchemaDef) schemas.ingest_frame(f);
    if (f.header.type == RecordType::SampleBlock)
      channels.ingest_frame(f, schemas);
  }
}

void put_fault(std::vector<uint8_t>& wire, uint32_t seq, uint64_t ts,
               uint16_t code, uint8_t severity, uint8_t subsystem,
               const std::vector<uint8_t>& detail) {
  ByteWriter p;
  p.u16(code);
  p.u8(severity);
  p.u8(subsystem);
  p.u8(static_cast<uint8_t>(detail.size()));
  p.bytes(detail);
  put_frame(wire, RecordType::Fault, 0x100, seq, ts, p.buf());
}

void put_transition(std::vector<uint8_t>& wire, uint32_t seq, uint64_t ts,
                    uint16_t from_state, uint16_t to_state, uint8_t cause) {
  ByteWriter p;
  p.u16(from_state);
  p.u16(to_state);
  p.u8(cause);
  p.u8(0);
  put_frame(wire, RecordType::StateTransition, 0x100, seq, ts, p.buf());
}

void build_anomalies(AnomalyEngine& engine) {
  std::vector<uint8_t> wire;
  put_fault(wire, 1, 1000, 16, 2 , 3, {0xDE, 0xAD});
  put_transition(wire, 2, 1050, 0, 5, 2);
  put_fault(wire, 3, 1100, 32, 0 , 1, {});
  engine.ingest(wire.data(), wire.size());
}

std::vector<Session> build_sessions() {
  Session a;
  a.device_id = 0x100;
  a.session_index = 0;
  a.device_class = 7;
  a.fw_major = 1;
  a.fw_minor = 2;
  a.fw_patch = 3;
  a.schema_epoch = 99;
  a.label = "clean";
  a.begin_seq = 1;
  a.begin_ts = 1000;
  a.end_seq = 9;
  a.end_ts = 5000;
  a.end_reason = 0;
  a.closed = true;

  Session b;
  b.device_id = 0x200;
  b.session_index = 1;
  b.device_class = 8;
  b.label = "a,b\"c";
  b.begin_seq = 10;
  b.begin_ts = 6000;
  b.closed = false;

  return {a, b};
}

bool ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(const std::string& s, const std::string& sub) {
  return s.find(sub) != std::string::npos;
}

size_t count_lines(const std::string& s) {
  size_t n = 0;
  for (char c : s) {
    if (c == '\n') ++n;
  }
  return n;
}

}

static void test_quote_field() {

  CHECK(quote_field("hello", DelimFormat::Csv) == "hello");
  CHECK(quote_field("", DelimFormat::Csv) == "");

  CHECK(quote_field("a,b", DelimFormat::Csv) == "\"a,b\"");

  CHECK(quote_field("a\"b", DelimFormat::Csv) == "\"a\"\"b\"");

  CHECK(quote_field("a\nb", DelimFormat::Csv) == "\"a\nb\"");
  CHECK(quote_field("a\rb", DelimFormat::Csv) == "\"a\rb\"");

  CHECK(quote_field("\"", DelimFormat::Csv) == "\"\"\"\"");

  CHECK(quote_field("hello", DelimFormat::Tsv) == "hello");

  CHECK(quote_field("a\tb", DelimFormat::Tsv) == "a\\tb");
  CHECK(quote_field("a\nb", DelimFormat::Tsv) == "a\\nb");
  CHECK(quote_field("a\rb", DelimFormat::Tsv) == "a\\rb");
  CHECK(quote_field("a\\b", DelimFormat::Tsv) == "a\\\\b");

  CHECK(quote_field("a,b", DelimFormat::Tsv) == "a,b");

  CHECK(delim_char(DelimFormat::Csv) == ',');
  CHECK(delim_char(DelimFormat::Tsv) == '\t');
}

static void test_channel_delimited() {
  SchemaRegistry schemas;
  ChannelStore channels;
  build_channels(schemas, channels);

  std::string csv;
  CHECK(export_channel_delimited(channels, schemas, 1, DelimFormat::Csv, csv) ==
        TableExportError::None);
  CHECK(csv ==
        "index,timestamp,raw\n"
        "0,5000,100\n"
        "1,5000,200\n"
        "2,5000,300\n");

  std::string tsv;
  CHECK(export_channel_delimited(channels, schemas, 1, DelimFormat::Tsv, tsv) ==
        TableExportError::None);
  CHECK(tsv ==
        "index\ttimestamp\traw\n"
        "0\t5000\t100\n"
        "1\t5000\t200\n"
        "2\t5000\t300\n");

  std::string csv2;
  CHECK(export_channel_delimited(channels, schemas, 2, DelimFormat::Csv, csv2) ==
        TableExportError::None);
  CHECK(csv2 ==
        "index,timestamp,\"a\"\"b\"\n"
        "0,7000,42\n");

  std::string bad = "leftover";
  CHECK(export_channel_delimited(channels, schemas, 99, DelimFormat::Csv,
                                 bad) == TableExportError::UnknownChannel);
  CHECK(bad.empty());

  std::string again;
  export_channel_delimited(channels, schemas, 1, DelimFormat::Csv, again);
  CHECK(again == csv);
}

static void test_faults_transitions_delimited() {
  AnomalyEngine engine;
  build_anomalies(engine);
  CHECK(engine.faults().size() == 2);
  CHECK(engine.transitions().size() == 1);

  std::string faults;
  CHECK(export_faults_delimited(engine, DelimFormat::Csv, faults) ==
        TableExportError::None);
  CHECK(faults ==
        "device,seq,ts,code,severity,subsystem,detail\n"
        "00000100,1,1000,16,error,3,dead\n"
        "00000100,3,1100,32,info,1,\n");

  std::string faults_tsv;
  export_faults_delimited(engine, DelimFormat::Tsv, faults_tsv);
  CHECK(faults_tsv ==
        "device\tseq\tts\tcode\tseverity\tsubsystem\tdetail\n"
        "00000100\t1\t1000\t16\terror\t3\tdead\n"
        "00000100\t3\t1100\t32\tinfo\t1\t\n");

  std::string trans;
  CHECK(export_transitions_delimited(engine, DelimFormat::Csv, trans) ==
        TableExportError::None);
  CHECK(trans ==
        "device,seq,ts,from_state,to_state,cause,continues_chain\n"
        "00000100,2,1050,0,5,2,true\n");
}

static void test_sessions_delimited() {
  std::vector<Session> sessions = build_sessions();

  std::string out;
  CHECK(export_sessions_delimited(sessions, DelimFormat::Csv, out) ==
        TableExportError::None);

  const std::string header =
      "device,session_index,device_class,fw_major,fw_minor,fw_patch,"
      "schema_epoch,label,begin_seq,begin_ts,end_seq,end_ts,closed,end_reason,"
      "duration_ns,checkpoints,reconnects,markers,seq_regressions,"
      "ts_regressions\n";
  CHECK(out.compare(0, header.size(), header) == 0);

  CHECK(contains(out, "00000100,0,7,1,2,3,99,clean,1,1000,9,5000,true,0,4000,"
                      "0,0,0,0,0\n"));

  CHECK(contains(out, "00000200,1,8,0,0,0,0,\"a,b\"\"c\",10,6000,0,0,false,0,"
                      "0,0,0,0,0,0\n"));
}

static void test_ndjson() {
  SchemaRegistry schemas;
  ChannelStore channels;
  build_channels(schemas, channels);

  std::string chan;
  CHECK(export_channel_ndjson(channels, schemas, 1, chan) ==
        TableExportError::None);
  CHECK(count_lines(chan) == 3);
  CHECK(chan.front() == '{');
  CHECK(ends_with(chan, "}\n"));

  CHECK(contains(chan, "{\"index\":0,\"timestamp\":5000,\"raw\":100}\n"));
  CHECK(contains(chan, "{\"index\":2,\"timestamp\":5000,\"raw\":300}\n"));

  std::string chan2;
  CHECK(export_channel_ndjson(channels, schemas, 2, chan2) ==
        TableExportError::None);
  CHECK(chan2 == "{\"index\":0,\"timestamp\":7000,\"a\\\"b\":42}\n");

  std::string bad = "x";
  CHECK(export_channel_ndjson(channels, schemas, 42, bad) ==
        TableExportError::UnknownChannel);
  CHECK(bad.empty());

  AnomalyEngine engine;
  build_anomalies(engine);
  std::string faults;
  CHECK(export_faults_ndjson(engine, faults) == TableExportError::None);
  CHECK(count_lines(faults) == 2);

  CHECK(contains(faults,
                 "{\"device_id\":256,\"session_seq\":1,\"monotonic_ts\":1000,"
                 "\"fault_code\":16,\"severity\":\"error\",\"subsystem_id\":3,"
                 "\"detail\":\"dead\",\"state_at_fault\":null}\n"));

  CHECK(contains(faults,
                 "{\"device_id\":256,\"session_seq\":3,\"monotonic_ts\":1100,"
                 "\"fault_code\":32,\"severity\":\"info\",\"subsystem_id\":1,"
                 "\"detail\":\"\",\"state_at_fault\":5}\n"));

  std::string again;
  export_faults_ndjson(engine, again);
  CHECK(again == faults);
}

static void test_error_strings() {
  CHECK(std::string(table_export_error_str(TableExportError::None)) == "ok");
  CHECK(std::string(table_export_error_str(TableExportError::UnknownChannel)) ==
        "unknown-channel");
  CHECK(std::string(table_export_error_str(TableExportError::BadField)) ==
        "bad-field");
}

int main() {
  test_quote_field();
  test_channel_delimited();
  test_faults_transitions_delimited();
  test_sessions_delimited();
  test_ndjson();
  test_error_strings();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all export tests passed\n");
  return 0;
}
