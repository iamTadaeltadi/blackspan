#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/byteio.h"
#include "blackspan/channel.h"
#include "blackspan/derive.h"
#include "blackspan/expr.h"
#include "blackspan/frame.h"
#include "blackspan/schema.h"

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

// The imu channel reused across the derive tests: a float, an unsigned counter,
// a byte flag, a signed temperature, and a wide altitude (stride 19).
// Field names/indices: ax=0, seq=1, ok=2, temp=3, alt=4.
static ChannelSchema imu_schema(uint16_t channel_id = 3) {
  return make_schema(channel_id, "imu",
                     {{"ax", FieldType::F32}, {"seq", FieldType::U32},
                      {"ok", FieldType::U8}, {"temp", FieldType::I16},
                      {"alt", FieldType::F64}});
}

struct ImuRow {
  float ax;
  uint32_t seq;
  uint8_t ok;
  int16_t temp;
  double alt;
};

static ImuRow imu_row(int i) {
  ImuRow r;
  r.ax = static_cast<float>(i) * 1.5f;
  r.seq = static_cast<uint32_t>(100 + i);
  r.ok = static_cast<uint8_t>(i % 2);
  r.temp = static_cast<int16_t>(-i * 10);
  r.alt = static_cast<double>(i) * 0.25;
  return r;
}

static void write_imu_row(ByteWriter& w, const ImuRow& r) {
  uint32_t axbits;
  std::memcpy(&axbits, &r.ax, 4);
  w.u32(axbits);
  w.u32(r.seq);
  w.u8(r.ok);
  w.u16(static_cast<uint16_t>(r.temp));
  uint64_t altbits;
  std::memcpy(&altbits, &r.alt, 8);
  w.u64(altbits);
}

static std::vector<uint8_t> imu_rows(int lo, int hi) {
  ByteWriter w;
  for (int i = lo; i < hi; ++i) write_imu_row(w, imu_row(i));
  return w.buf();
}

static std::vector<uint8_t> build_block_payload(
    uint16_t channel_id, uint16_t version, uint32_t base_ts_delta,
    uint16_t sample_count, uint16_t flags, const std::vector<uint8_t>& rows) {
  ByteWriter w;
  w.u16(channel_id);
  w.u16(version);
  w.u32(base_ts_delta);
  w.u16(sample_count);
  w.u16(flags);
  w.bytes(rows);
  return w.buf();
}

static Frame make_block_frame(uint64_t monotonic_ts,
                              const std::vector<uint8_t>& payload) {
  Frame f;
  f.header.type = RecordType::SampleBlock;
  f.header.monotonic_ts = monotonic_ts;
  f.payload = payload;
  return f;
}

// Encodes a derived-def payload per the wire grammar.
static std::vector<uint8_t> build_derived_payload(uint16_t derived_id,
                                                  uint16_t primary_channel,
                                                  const std::string& name,
                                                  const std::string& expr) {
  ByteWriter w;
  w.u16(derived_id);
  w.u16(primary_channel);
  w.u8(static_cast<uint8_t>(name.size()));
  w.bytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
  w.u16(static_cast<uint16_t>(expr.size()));
  w.bytes(reinterpret_cast<const uint8_t*>(expr.data()), expr.size());
  return w.buf();
}

// A binding that resolves a fixed table of bare (primary-channel) identifiers.
static ExprBinding table_binding(
    const std::vector<std::pair<std::string, double>>& table) {
  return [table](const FieldReference& r) -> std::optional<double> {
    if (r.cross) return std::nullopt;
    for (const auto& kv : table) {
      if (kv.first == r.name) return kv.second;
    }
    return std::nullopt;
  };
}

// A binding used for pure-numeric expressions: no reference ever resolves.
static ExprBinding no_fields() {
  return [](const FieldReference&) -> std::optional<double> {
    return std::nullopt;
  };
}

static void test_decode_derived_def() {
  const std::vector<uint8_t> p =
      build_derived_payload(0x0011, 0x0203, "accel_g", "ax * 0.00125");
  DerivedDefRecord rec;
  CHECK(decode_derived_def(p.data(), p.size(), rec) == DeriveError::None);
  CHECK(rec.derived_id == 0x0011);
  CHECK(rec.primary_channel == 0x0203);
  CHECK(rec.name == "accel_g");
  CHECK(rec.expr == "ax * 0.00125");

  // Empty name and empty expr round-trip cleanly.
  const std::vector<uint8_t> q = build_derived_payload(7, 3, "", "");
  DerivedDefRecord rq;
  CHECK(decode_derived_def(q.data(), q.size(), rq) == DeriveError::None);
  CHECK(rq.derived_id == 7 && rq.primary_channel == 3);
  CHECK(rq.name.empty() && rq.expr.empty());

  // Truncation at every boundary is reported, never crashes.
  DerivedDefRecord junk;
  CHECK(decode_derived_def(nullptr, 0, junk) == DeriveError::Truncated);
  for (size_t cut = 0; cut < p.size(); ++cut) {
    std::vector<uint8_t> t(p.begin(), p.begin() + cut);
    CHECK(decode_derived_def(t.data(), t.size(), junk) ==
          DeriveError::Truncated);
  }
  // Trailing bytes after a complete record are tolerated (ignored).
  std::vector<uint8_t> extra = p;
  extra.push_back(0xAB);
  extra.push_back(0xCD);
  DerivedDefRecord re;
  CHECK(decode_derived_def(extra.data(), extra.size(), re) ==
        DeriveError::None);
  CHECK(re.expr == "ax * 0.00125");
}

static void test_precedence_and_unary() {
  const ExprBinding b = no_fields();

  CHECK(evaluate_expr_once("2 + 3 * 4", b).value == 14.0);
  CHECK(evaluate_expr_once("(2 + 3) * 4", b).value == 20.0);
  CHECK(evaluate_expr_once("2 * 3 + 4", b).value == 10.0);
  CHECK(evaluate_expr_once("20 - 4 - 3", b).value == 13.0);   // left assoc
  CHECK(evaluate_expr_once("100 / 5 / 2", b).value == 10.0);  // left assoc
  CHECK(evaluate_expr_once("-3 + 4", b).value == 1.0);
  CHECK(evaluate_expr_once("2 * -3", b).value == -6.0);
  CHECK(evaluate_expr_once("--5", b).value == 5.0);       // double negation
  CHECK(evaluate_expr_once("-(2 + 3)", b).value == -5.0);
  CHECK(evaluate_expr_once("+7", b).value == 7.0);        // unary plus
  CHECK(evaluate_expr_once("2 + 3 * 4 - 10 / 2", b).value == 9.0);

  const EvalOnceResult r = evaluate_expr_once("1.5e2 + 0.5", b);
  CHECK(r.ok());
  CHECK(r.value == 150.5);
}

static void test_functions() {
  const ExprBinding b = no_fields();
  CHECK(evaluate_expr_once("abs(-5)", b).value == 5.0);
  CHECK(evaluate_expr_once("abs(5)", b).value == 5.0);
  CHECK(evaluate_expr_once("sqrt(9)", b).value == 3.0);
  CHECK(evaluate_expr_once("sqrt(2) * sqrt(2)", b).value ==
        std::sqrt(2.0) * std::sqrt(2.0));
  CHECK(evaluate_expr_once("floor(2.7)", b).value == 2.0);
  CHECK(evaluate_expr_once("floor(-2.1)", b).value == -3.0);
  CHECK(evaluate_expr_once("ceil(2.1)", b).value == 3.0);
  CHECK(evaluate_expr_once("ceil(-2.9)", b).value == -2.0);
  CHECK(evaluate_expr_once("min(3, 7)", b).value == 3.0);
  CHECK(evaluate_expr_once("min(7, 3)", b).value == 3.0);
  CHECK(evaluate_expr_once("max(3, 7)", b).value == 7.0);
  CHECK(evaluate_expr_once("max(-3, -7)", b).value == -3.0);
  CHECK(evaluate_expr_once("clamp(5, 0, 10)", b).value == 5.0);
  CHECK(evaluate_expr_once("clamp(-1, 0, 10)", b).value == 0.0);
  CHECK(evaluate_expr_once("clamp(20, 0, 10)", b).value == 10.0);
  // Nested calls and mixed arithmetic.
  CHECK(evaluate_expr_once("max(abs(-4), min(2, 9))", b).value == 4.0);
  CHECK(evaluate_expr_once("clamp(sqrt(16) + 1, 0, 4)", b).value == 4.0);
}

static void test_div_by_zero_nan() {
  const ExprBinding b = no_fields();
  CHECK(std::isnan(evaluate_expr_once("1 / 0", b).value));
  CHECK(std::isnan(evaluate_expr_once("0 / 0", b).value));
  CHECK(std::isnan(evaluate_expr_once("(3 + 4) / (2 - 2)", b).value));
  // A zero divisor is not a failure: the parse and bind both succeed.
  const EvalOnceResult r = evaluate_expr_once("5 / 0", b);
  CHECK(r.ok());
  CHECK(std::isnan(r.value));

  // Division by a field that happens to be zero also yields NaN.
  const ExprBinding fb = table_binding({{"num", 5.0}, {"den", 0.0}});
  CHECK(std::isnan(evaluate_expr_once("num / den", fb).value));
}

static void test_field_binding_helper() {
  const ExprBinding b = table_binding({{"raw", 800.0}, {"offset", 1.5}});
  const EvalOnceResult r = evaluate_expr_once("raw * 0.00125 + offset", b);
  CHECK(r.ok());
  CHECK(r.value == 800.0 * 0.00125 + 1.5);

  // A reference the binding cannot resolve is reported, not crashed.
  const EvalOnceResult miss = evaluate_expr_once("raw + gone", b);
  CHECK(!miss.ok());
  CHECK(miss.parse_error == ExprError::None);
  CHECK(miss.bind_failed);
  CHECK(!miss.missing.cross && miss.missing.name == "gone");
}

static void test_parse_errors() {
  struct Case {
    const char* text;
    ExprError err;
  };
  const Case cases[] = {
      {"", ExprError::Empty},
      {"(2 + 3", ExprError::UnbalancedParen},
      {"2 + 3)", ExprError::UnbalancedParen},
      {"nope(1)", ExprError::UnknownFunction},
      {"min(1)", ExprError::BadArity},
      {"abs(1, 2)", ExprError::BadArity},
      {"clamp(1, 2)", ExprError::BadArity},
      {"2 +", ExprError::UnexpectedEnd},
      {"* 3", ExprError::UnexpectedToken},
      {"2 3", ExprError::TrailingTokens},
      {"2 @ 3", ExprError::UnexpectedToken},
      {"bad.field", ExprError::BadChannelRef},
      {"chx.field", ExprError::BadChannelRef},
      {"ch3.", ExprError::UnexpectedEnd},
  };
  for (const Case& c : cases) {
    const ParseResult r = parse_expr(c.text);
    CHECK(!r.ok());
    CHECK(r.error == c.err);
    if (r.error != c.err) {
      std::fprintf(stderr, "  (expr \"%s\" gave %s)\n", c.text,
                   expr_error_str(r.error));
    }
  }

  // The reported position points into the source text.
  const ParseResult unb = parse_expr("(2 + 3");
  CHECK(unb.error == ExprError::UnbalancedParen);
  CHECK(unb.position == 0);  // the unmatched '('
  const ParseResult trail = parse_expr("2 3");
  CHECK(trail.error == ExprError::TrailingTokens);
  CHECK(trail.position == 2);  // the stray '3'

  // A pathologically deep expression is refused, not crashed.
  std::string deep;
  for (int i = 0; i < 5000; ++i) deep.push_back('(');
  deep.push_back('1');
  for (int i = 0; i < 5000; ++i) deep.push_back(')');
  const ParseResult dr = parse_expr(deep);
  CHECK(dr.error == ExprError::TooComplex);
}

// Builds a store with two imu blocks on channel 3: A=[0,1] at ts 1000 and
// B=[2,3,4] at ts 2000 (5 samples total).
static void seed_imu(SchemaRegistry& reg, ChannelStore& store) {
  reg.register_schema(imu_schema());
  store.ingest_frame(
      make_block_frame(1000, build_block_payload(3, 1, 0, 2, 0, imu_rows(0, 2))),
      reg);
  store.ingest_frame(
      make_block_frame(2000, build_block_payload(3, 1, 0, 3, 0, imu_rows(2, 5))),
      reg);
}

static void test_engine_evaluate() {
  SchemaRegistry reg;
  ChannelStore store;
  seed_imu(reg, store);

  DeriveEngine eng;
  DerivedDefRecord def;
  def.derived_id = 1;
  def.primary_channel = 3;
  def.name = "accel_scaled";
  def.expr = "ax * 0.00125 + seq";
  ExprError perr = ExprError::UnexpectedToken;
  size_t ppos = 99;
  CHECK(eng.register_def(def, &perr, &ppos) == DeriveError::None);
  CHECK(perr == ExprError::None && ppos == 0);
  CHECK(eng.count() == 1);
  CHECK(eng.get(1) != nullptr && eng.get(1)->name == "accel_scaled");
  CHECK(eng.get(2) == nullptr);
  const std::vector<uint16_t> ids = eng.defs();
  CHECK(ids.size() == 1 && ids[0] == 1);

  DerivedSeries out;
  CHECK(eng.evaluate(1, store, out) == DeriveError::None);
  CHECK(out.derived_id == 1);
  CHECK(out.values.size() == 5);
  CHECK(out.timestamps.size() == out.values.size());
  for (int i = 0; i < 5; ++i) {
    const ImuRow r = imu_row(i);
    const double expected =
        static_cast<double>(r.ax) * 0.00125 + static_cast<double>(r.seq);
    CHECK(out.values[static_cast<size_t>(i)] == expected);
  }
  // Timestamps follow the primary blocks: A -> 1000, B -> 2000.
  CHECK(out.timestamps[0] == 1000 && out.timestamps[1] == 1000);
  CHECK(out.timestamps[2] == 2000 && out.timestamps[3] == 2000 &&
        out.timestamps[4] == 2000);

  // A precedence-sensitive formula evaluated per sample.
  DerivedDefRecord def2;
  def2.derived_id = 2;
  def2.primary_channel = 3;
  def2.name = "combo";
  def2.expr = "seq + ax * 2 - 1";
  CHECK(eng.register_def(def2) == DeriveError::None);
  DerivedSeries out2;
  CHECK(eng.evaluate(2, store, out2) == DeriveError::None);
  for (int i = 0; i < 5; ++i) {
    const ImuRow r = imu_row(i);
    const double expected =
        static_cast<double>(r.seq) + static_cast<double>(r.ax) * 2.0 - 1.0;
    CHECK(out2.values[static_cast<size_t>(i)] == expected);
  }

  // A function over a field: clamp the altitude into [0, 0.6].
  DerivedDefRecord def3;
  def3.derived_id = 3;
  def3.primary_channel = 3;
  def3.expr = "clamp(alt, 0, 0.6)";
  CHECK(eng.register_def(def3) == DeriveError::None);
  DerivedSeries out3;
  CHECK(eng.evaluate(3, store, out3) == DeriveError::None);
  for (int i = 0; i < 5; ++i) {
    const double alt = imu_row(i).alt;
    const double expected = alt < 0.0 ? 0.0 : (alt > 0.6 ? 0.6 : alt);
    CHECK(out3.values[static_cast<size_t>(i)] == expected);
  }
}

static void test_div_by_zero_in_series() {
  SchemaRegistry reg;
  ChannelStore store;
  seed_imu(reg, store);

  // ok = i % 2, so even-indexed samples divide by zero -> NaN, and remain
  // non-fatal for the definition as a whole.
  DeriveEngine eng;
  DerivedDefRecord def;
  def.derived_id = 10;
  def.primary_channel = 3;
  def.expr = "ax / ok";
  CHECK(eng.register_def(def) == DeriveError::None);
  DerivedSeries out;
  CHECK(eng.evaluate(10, store, out) == DeriveError::None);
  CHECK(out.values.size() == 5);
  for (int i = 0; i < 5; ++i) {
    if (i % 2 == 0) {
      CHECK(std::isnan(out.values[static_cast<size_t>(i)]));
    } else {
      CHECK(out.values[static_cast<size_t>(i)] ==
            static_cast<double>(imu_row(i).ax) / 1.0);
    }
  }
}

static void test_cross_channel() {
  SchemaRegistry reg;
  reg.register_schema(imu_schema(3));
  reg.register_schema(
      make_schema(5, "power", {{"v", FieldType::U16}, {"cur", FieldType::I16}}));

  ChannelStore store;
  // imu channel 3: two samples.
  CHECK(store.ingest_frame(
            make_block_frame(500, build_block_payload(3, 1, 0, 2, 0,
                                                      imu_rows(0, 2))),
            reg) == ChannelError::None);
  // power channel 5: two samples (v, cur) = (10, -1), (20, -2).
  ByteWriter pw;
  pw.u16(10);
  pw.u16(static_cast<uint16_t>(-1));
  pw.u16(20);
  pw.u16(static_cast<uint16_t>(-2));
  CHECK(store.ingest_frame(
            make_block_frame(700, build_block_payload(5, 1, 0, 2, 0, pw.buf())),
            reg) == ChannelError::None);

  DeriveEngine eng;
  DerivedDefRecord def;
  def.derived_id = 1;
  def.primary_channel = 3;
  def.name = "ax_plus_v";
  def.expr = "ax + ch5.v";
  CHECK(eng.register_def(def) == DeriveError::None);
  DerivedSeries out;
  CHECK(eng.evaluate(1, store, out) == DeriveError::None);
  CHECK(out.values.size() == 2);
  const double v[2] = {10.0, 20.0};
  for (int i = 0; i < 2; ++i) {
    const double expected = static_cast<double>(imu_row(i).ax) + v[i];
    CHECK(out.values[static_cast<size_t>(i)] == expected);
  }

  // A cross-channel reference to a channel shorter than the primary is an
  // error for the definition. Compact power to one sample; imu still has two.
  CHECK(store.compact_channel(5, 1) == ChannelError::None);
  DerivedSeries out2;
  CHECK(eng.evaluate(1, store, out2) == DeriveError::ChannelTooShort);
  CHECK(out2.values.empty() && out2.timestamps.empty());
}

static void test_eval_errors() {
  SchemaRegistry reg;
  ChannelStore store;
  seed_imu(reg, store);
  DeriveEngine eng;

  // An unknown bare identifier is only an error at evaluation time.
  {
    DerivedDefRecord def;
    def.derived_id = 1;
    def.primary_channel = 3;
    def.expr = "ax + missing";
    CHECK(eng.register_def(def) == DeriveError::None);  // parses fine
    DerivedSeries out;
    CHECK(eng.evaluate(1, store, out) == DeriveError::UnknownFieldRef);
    CHECK(out.values.empty());
  }
  // An unknown cross-channel id.
  {
    DerivedDefRecord def;
    def.derived_id = 2;
    def.primary_channel = 3;
    def.expr = "ch99.x";
    CHECK(eng.register_def(def) == DeriveError::None);
    DerivedSeries out;
    CHECK(eng.evaluate(2, store, out) == DeriveError::UnknownFieldRef);
  }
  // Evaluating an unregistered id.
  {
    DerivedSeries out;
    CHECK(eng.evaluate(999, store, out) == DeriveError::UnknownId);
  }
  // A primary channel with no series in the store.
  {
    DerivedDefRecord def;
    def.derived_id = 3;
    def.primary_channel = 77;  // never ingested
    def.expr = "1 + 2";
    CHECK(eng.register_def(def) == DeriveError::None);
    DerivedSeries out;
    CHECK(eng.evaluate(3, store, out) == DeriveError::NoPrimaryChannel);
  }
  // A malformed expression fails at registration with position info.
  {
    DerivedDefRecord def;
    def.derived_id = 4;
    def.primary_channel = 3;
    def.expr = "ax * (2 + )";
    ExprError perr = ExprError::None;
    size_t ppos = 0;
    CHECK(eng.register_def(def, &perr, &ppos) == DeriveError::ParseFailed);
    CHECK(perr == ExprError::UnexpectedToken);
    CHECK(eng.get(4) == nullptr);  // not stored
  }
  // Registering the same id twice is refused.
  {
    DerivedDefRecord def;
    def.derived_id = 1;  // already registered above
    def.primary_channel = 3;
    def.expr = "seq";
    CHECK(eng.register_def(def) == DeriveError::DuplicateId);
  }
}

static void test_ingest_frame_wire() {
  SchemaRegistry reg;
  ChannelStore store;
  seed_imu(reg, store);

  Frame f;
  f.header.type = RecordType::DerivedDef;
  f.payload = build_derived_payload(42, 3, "temp_c", "temp * 0.1");

  // Wrong record type is rejected.
  Frame wrong = f;
  wrong.header.type = RecordType::SampleBlock;
  DeriveEngine eng0;
  CHECK(eng0.ingest_frame(wrong) == DeriveError::NotDerivedDef);

  // A truncated derived-def payload surfaces the decode error.
  Frame broken = f;
  broken.payload = {1, 0, 2};  // stops inside the fixed header
  CHECK(eng0.ingest_frame(broken) == DeriveError::Truncated);

  // A full frame encode/decode round trip feeds ingest_frame from the wire.
  std::vector<uint8_t> wire;
  encode_frame(f, wire);
  Frame decoded;
  size_t consumed = 0;
  CHECK(decode_frame(wire.data(), wire.size(), decoded, &consumed) ==
        FrameStatus::Ok);

  DeriveEngine eng;
  CHECK(eng.ingest_frame(decoded) == DeriveError::None);
  CHECK(eng.get(42) != nullptr);
  CHECK(eng.get(42)->name == "temp_c");
  CHECK(eng.get(42)->expr == "temp * 0.1");

  DerivedSeries out;
  CHECK(eng.evaluate(42, store, out) == DeriveError::None);
  CHECK(out.values.size() == 5);
  for (int i = 0; i < 5; ++i) {
    const double expected = static_cast<double>(imu_row(i).temp) * 0.1;
    CHECK(out.values[static_cast<size_t>(i)] == expected);
  }
}

static void test_error_strings() {
  CHECK(std::strcmp(derive_error_str(DeriveError::None), "ok") == 0);
  CHECK(std::strcmp(derive_error_str(DeriveError::ChannelTooShort),
                    "channel-too-short") == 0);
  CHECK(std::strcmp(expr_error_str(ExprError::None), "ok") == 0);
  CHECK(std::strcmp(expr_error_str(ExprError::BadArity), "bad-arity") == 0);
  CHECK(std::strcmp(func_name(FuncId::Clamp), "clamp") == 0);
  CHECK(std::strcmp(func_name(FuncId::Sqrt), "sqrt") == 0);
}

static void test_empty_primary_series() {
  // A primary channel with a series but zero retained samples evaluates to an
  // empty (but successful) derived series.
  SchemaRegistry reg;
  ChannelStore store;
  seed_imu(reg, store);
  CHECK(store.compact_channel(3, 0) == ChannelError::None);

  DeriveEngine eng;
  DerivedDefRecord def;
  def.derived_id = 1;
  def.primary_channel = 3;
  def.expr = "ax + 1";
  CHECK(eng.register_def(def) == DeriveError::None);
  DerivedSeries out;
  CHECK(eng.evaluate(1, store, out) == DeriveError::None);
  CHECK(out.values.empty());
  CHECK(out.timestamps.empty());
  CHECK(out.derived_id == 1);
}

int main() {
  test_decode_derived_def();
  test_precedence_and_unary();
  test_functions();
  test_div_by_zero_nan();
  test_field_binding_helper();
  test_parse_errors();
  test_engine_evaluate();
  test_div_by_zero_in_series();
  test_cross_channel();
  test_eval_errors();
  test_ingest_frame_wire();
  test_error_strings();
  test_empty_primary_series();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all derive tests passed\n");
  return 0;
}
