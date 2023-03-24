#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/byteio.h"
#include "blackspan/channel.h"
#include "blackspan/frame.h"
#include "blackspan/query.h"
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

void build_channels(SchemaRegistry& schemas, ChannelStore& channels) {
  std::vector<uint8_t> wire;
  {
    ByteWriter p;
    p.u16(1);
    p.u8(1);
    p.u8(1);
    const std::string cname = "temp";
    p.u8(static_cast<uint8_t>(cname.size()));
    p.bytes(reinterpret_cast<const uint8_t*>(cname.data()), cname.size());
    p.u8(3);
    const std::string fname = "raw";
    p.u8(static_cast<uint8_t>(fname.size()));
    p.bytes(reinterpret_cast<const uint8_t*>(fname.data()), fname.size());
    put_frame(wire, RecordType::SchemaDef, 0x100, 0, 1000, p.buf());
  }
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
    put_frame(wire, RecordType::SampleBlock, 0x100, 1, 1001, p.buf());
  }
  FrameStream fs(wire.data(), wire.size());
  Frame f;
  while (fs.next(f)) {
    if (f.header.type == RecordType::SchemaDef) schemas.ingest_frame(f);
    if (f.header.type == RecordType::SampleBlock)
      channels.ingest_frame(f, schemas);
  }
}

void add_fault(std::vector<uint8_t>& wire, uint32_t seq, uint64_t ts,
               uint16_t code, uint8_t severity) {
  ByteWriter p;
  p.u16(code);
  p.u8(severity);
  p.u8(1);
  p.u8(0);
  put_frame(wire, RecordType::Fault, 0x100, seq, ts, p.buf());
}

void add_transition(std::vector<uint8_t>& wire, uint32_t seq, uint64_t ts,
                    uint16_t from, uint16_t to, uint8_t cause) {
  ByteWriter p;
  p.u16(from);
  p.u16(to);
  p.u8(cause);
  p.u8(0);
  put_frame(wire, RecordType::StateTransition, 0x100, seq, ts, p.buf());
}

void build_anomalies(AnomalyEngine& engine) {
  std::vector<uint8_t> wire;
  add_transition(wire, 0, 1000, 0, 1, 5);
  add_transition(wire, 1, 1010, 1, 2, 6);
  add_fault(wire, 2, 1100, 10, 0);
  add_fault(wire, 3, 1200, 11, 1);
  add_fault(wire, 4, 1300, 12, 2);
  add_fault(wire, 5, 1400, 42, 3);
  engine.ingest(wire.data(), wire.size());
}

void put_begin(std::vector<uint8_t>& wire, uint32_t dev, uint32_t seq,
               uint64_t ts, const std::string& label) {
  ByteWriter w;
  w.u16(1);
  w.u8(0);
  w.u8(0);
  w.u16(0);
  w.u32(0);
  w.u8(static_cast<uint8_t>(label.size()));
  w.bytes(reinterpret_cast<const uint8_t*>(label.data()), label.size());
  put_frame(wire, RecordType::SessionBegin, dev, seq, ts, w.buf());
}

void put_end(std::vector<uint8_t>& wire, uint32_t dev, uint32_t seq,
             uint64_t ts) {
  ByteWriter w;
  w.u16(0);
  put_frame(wire, RecordType::SessionEnd, dev, seq, ts, w.buf());
}

void build_sessions(SessionReconstructor& r) {
  std::vector<uint8_t> wire;
  put_begin(wire, 7, 0, 100, "alpha");
  put_end(wire, 7, 1, 200);
  put_begin(wire, 7, 2, 300, "beta");
  r.reconstruct(wire.data(), wire.size());
}

std::string col(const QueryResultRow& r, const std::string& name) {
  for (const auto& c : r.columns) {
    if (c.first == name) return c.second;
  }
  return "<none>";
}

}

static void test_faults_predicates() {
  AnomalyEngine engine;
  build_anomalies(engine);
  QueryEngine qe(nullptr, nullptr, &engine, nullptr);
  QueryResult out;

  CHECK(qe.run("select faults where severity >= error", out) ==
        QueryError::None);
  CHECK(out.entity == "faults");
  CHECK(out.rows.size() == 2);
  CHECK(col(out.rows[0], "severity") == "error");
  CHECK(col(out.rows[1], "severity") == "critical");

  CHECK(qe.run("select faults where device == 0x100 and code == 42", out) ==
        QueryError::None);
  CHECK(out.rows.size() == 1);
  CHECK(col(out.rows[0], "code") == "42");
  CHECK(col(out.rows[0], "device") == "256");

  CHECK(qe.run("select faults where severity == info or severity == critical",
               out) == QueryError::None);
  CHECK(out.rows.size() == 2);

  CHECK(qe.run("select faults where not code == 10", out) == QueryError::None);
  CHECK(out.rows.size() == 3);

  CHECK(qe.run(
            "select faults where (code == 10 or code == 11) and severity < "
            "error",
            out) == QueryError::None);
  CHECK(out.rows.size() == 2);

  CHECK(qe.run("select faults limit 2", out) == QueryError::None);
  CHECK(out.rows.size() == 2);

  CHECK(qe.run("select faults where code == 999", out) == QueryError::None);
  CHECK(out.rows.empty());

  CHECK(qe.run("select faults", out) == QueryError::None);
  CHECK(out.rows.size() == 4);
}

static void test_transitions() {
  AnomalyEngine engine;
  build_anomalies(engine);
  QueryEngine qe(nullptr, nullptr, &engine, nullptr);
  QueryResult out;

  CHECK(qe.run("select transitions", out) == QueryError::None);
  CHECK(out.entity == "transitions");
  CHECK(out.rows.size() == 2);

  CHECK(qe.run("select transitions where to == 2", out) == QueryError::None);
  CHECK(out.rows.size() == 1);
  CHECK(col(out.rows[0], "from") == "1");
  CHECK(col(out.rows[0], "cause") == "6");

  CHECK(qe.run("select transitions limit 1", out) == QueryError::None);
  CHECK(out.rows.size() == 1);
}

static void test_sessions() {
  SessionReconstructor r;
  build_sessions(r);
  QueryEngine qe(&r.sessions(), nullptr, nullptr, nullptr);
  QueryResult out;

  CHECK(qe.run("select sessions", out) == QueryError::None);
  CHECK(out.rows.size() == 2);

  CHECK(qe.run("select sessions where closed == 1", out) == QueryError::None);
  CHECK(out.rows.size() == 1);
  CHECK(col(out.rows[0], "label") == "alpha");
  CHECK(col(out.rows[0], "duration") == "100");

  CHECK(qe.run("select sessions where label == 'alpha'", out) ==
        QueryError::None);
  CHECK(out.rows.size() == 1);

  CHECK(qe.run("select sessions where duration > 50", out) == QueryError::None);
  CHECK(out.rows.size() == 1);
  CHECK(col(out.rows[0], "index") == "0");
}

static void test_channels_and_samples() {
  SchemaRegistry schemas;
  ChannelStore channels;
  build_channels(schemas, channels);
  QueryEngine qe(nullptr, &channels, nullptr, &schemas);
  QueryResult out;

  CHECK(qe.run("select channels", out) == QueryError::None);
  CHECK(out.entity == "channels");
  CHECK(out.rows.size() == 1);
  CHECK(col(out.rows[0], "id") == "1");
  CHECK(col(out.rows[0], "samples") == "3");
  CHECK(col(out.rows[0], "name") == "temp");
  CHECK(col(out.rows[0], "versions") == "1");

  CHECK(qe.run("select channels where id == 2", out) == QueryError::None);
  CHECK(out.rows.empty());

  CHECK(qe.run("select samples from channel 1 where raw > 200", out) ==
        QueryError::None);
  CHECK(out.entity == "samples");
  CHECK(out.rows.size() == 1);
  CHECK(col(out.rows[0], "raw") == "300");
  CHECK(col(out.rows[0], "index") == "2");
  CHECK(col(out.rows[0], "ts") == "1001");

  CHECK(qe.run("select samples from channel 1", out) == QueryError::None);
  CHECK(out.rows.size() == 3);

  CHECK(qe.run("select samples from channel 1 where index == 0", out) ==
        QueryError::None);
  CHECK(out.rows.size() == 1);
  CHECK(col(out.rows[0], "raw") == "100");

  CHECK(qe.run("select samples from channel 1 limit 2", out) ==
        QueryError::None);
  CHECK(out.rows.size() == 2);

  CHECK(qe.run("select samples from channel 9 where raw > 1", out) ==
        QueryError::NoSuchChannel);

  CHECK(qe.run("select samples from channel 1 where bogus > 1", out) ==
        QueryError::UnknownAttr);
}

static void test_null_sources() {
  QueryEngine qe(nullptr, nullptr, nullptr, nullptr);
  QueryResult out;
  CHECK(qe.run("select faults", out) == QueryError::None);
  CHECK(out.rows.empty());
  CHECK(qe.run("select sessions", out) == QueryError::None);
  CHECK(out.rows.empty());
  CHECK(qe.run("select channels", out) == QueryError::None);
  CHECK(out.rows.empty());
}

static void test_parse_errors() {
  struct Case {
    std::string q;
    QueryError err;
    size_t pos;
  };
  const std::string bad_op = "select faults where code = 1";
  const std::string bad_val = "select faults where code == abc";
  const std::string un_end = "select faults where code ==";
  const std::string un_str = "select sessions where label == 'flight";
  const std::string miss_ch = "select samples where raw > 1";
  const std::string trailing = "select channels foo";
  const std::string unk_ent = "select widgets";
  const std::string unk_attr = "select faults where bogus == 1";
  const std::string unexp_tok = "select 123";

  const Case cases[] = {
      {"", QueryError::Empty, 0},
      {unk_ent, QueryError::UnknownEntity, unk_ent.find("widgets")},
      {unk_attr, QueryError::UnknownAttr, unk_attr.find("bogus")},
      {bad_op, QueryError::BadOperator, bad_op.find('=')},
      {bad_val, QueryError::BadValue, bad_val.find("abc")},
      {un_end, QueryError::UnexpectedEnd, un_end.size()},
      {un_str, QueryError::UnterminatedString, un_str.find('\'')},
      {miss_ch, QueryError::MissingChannel, miss_ch.find("where")},
      {trailing, QueryError::TrailingTokens, trailing.find("foo")},
      {unexp_tok, QueryError::UnexpectedToken, unexp_tok.find("123")},
  };

  for (const Case& c : cases) {
    ParsedQuery pq;
    size_t pos = 999;
    const QueryError e = parse_query(c.q, pq, &pos);
    CHECK(e == c.err);
    if (c.err == QueryError::Empty) {
      CHECK(pos == 0);
    } else {
      CHECK(pos == c.pos);
    }
  }
}

static void test_error_strings() {
  CHECK(std::string(query_error_str(QueryError::None)) == "ok");
  CHECK(std::string(query_error_str(QueryError::MissingChannel)) ==
        "missing-channel");
  CHECK(std::string(query_error_str(QueryError::NoSuchChannel)) ==
        "no-such-channel");
  CHECK(std::string(query_entity_str(QueryEntity::Samples)) == "samples");
}

int main() {
  test_faults_predicates();
  test_transitions();
  test_sessions();
  test_channels_and_samples();
  test_null_sources();
  test_parse_errors();
  test_error_strings();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all query tests passed\n");
  return 0;
}
