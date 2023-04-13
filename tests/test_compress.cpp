#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "blackspan/byteio.h"
#include "blackspan/channel.h"
#include "blackspan/compress.h"
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

static bool same_bits(double a, double b) {
  uint64_t ua = 0, ub = 0;
  std::memcpy(&ua, &a, 8);
  std::memcpy(&ub, &b, 8);
  return ua == ub;
}

static bool u64_roundtrips(const std::vector<uint64_t>& v, ColumnCodec codec) {
  const EncodedColumn enc = encode_u64_column(v, codec);
  if (enc.count != v.size()) return false;
  std::vector<uint64_t> out;
  if (!decode_u64_column(enc, out)) return false;
  return out == v;
}

static bool i64_roundtrips(const std::vector<int64_t>& v, ColumnCodec codec) {
  const EncodedColumn enc = encode_i64_column(v, codec);
  if (enc.count != v.size()) return false;
  std::vector<int64_t> out;
  if (!decode_i64_column(enc, out)) return false;
  return out == v;
}

static bool f64_roundtrips(const std::vector<double>& v) {
  const EncodedColumn enc = encode_f64_column(v);
  if (enc.count != v.size()) return false;
  if (enc.codec != ColumnCodec::DoubleSplit) return false;
  std::vector<double> out;
  if (!decode_f64_column(enc, out)) return false;
  if (out.size() != v.size()) return false;
  for (size_t i = 0; i < v.size(); ++i) {
    if (!same_bits(out[i], v[i])) return false;
  }
  return true;
}

static uint64_t splitmix(uint64_t& state) {
  state += 0x9E3779B97F4A7C15ull;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

static const ColumnCodec kIntCodecs[] = {ColumnCodec::Raw,
                                         ColumnCodec::DeltaZigzagVarint,
                                         ColumnCodec::Rle};

static void test_u64_roundtrip() {
  std::vector<std::vector<uint64_t>> cases;
  cases.push_back({});
  cases.push_back({42});
  cases.push_back(std::vector<uint64_t>(200, 7));
  {
    std::vector<uint64_t> ramp;
    for (uint64_t i = 0; i < 300; ++i) ramp.push_back(1000 + i);
    cases.push_back(ramp);
  }
  {
    std::vector<uint64_t> rnd;
    uint64_t st = 12345;
    for (int i = 0; i < 256; ++i) rnd.push_back(splitmix(st));
    cases.push_back(rnd);
  }
  cases.push_back({0, std::numeric_limits<uint64_t>::max(), 1,
                   std::numeric_limits<uint64_t>::max() - 1, 0x8000000000000000ull});
  {
    std::vector<uint64_t> down;
    for (uint64_t i = 0; i < 100; ++i) down.push_back(500000 - i * 3);
    cases.push_back(down);
  }

  for (const auto& c : cases) {
    for (const ColumnCodec codec : kIntCodecs) {
      CHECK(u64_roundtrips(c, codec));
    }
    CHECK(u64_roundtrips(c, ColumnCodec::DoubleSplit));
  }
}

static void test_i64_roundtrip() {
  std::vector<std::vector<int64_t>> cases;
  cases.push_back({});
  cases.push_back({-1});
  cases.push_back(std::vector<int64_t>(150, -9));
  {
    std::vector<int64_t> ramp;
    for (int64_t i = 0; i < 200; ++i) ramp.push_back(-100 + i);
    cases.push_back(ramp);
  }
  {
    std::vector<int64_t> rnd;
    uint64_t st = 999;
    for (int i = 0; i < 200; ++i) {
      rnd.push_back(static_cast<int64_t>(splitmix(st)));
    }
    cases.push_back(rnd);
  }
  cases.push_back({std::numeric_limits<int64_t>::min(),
                   std::numeric_limits<int64_t>::max(), 0, -1, 1,
                   std::numeric_limits<int64_t>::min() + 1,
                   std::numeric_limits<int64_t>::max() - 1});
  cases.push_back({-5, -5, -5, 4, 4, 100, 100, 100, 100});

  for (const auto& c : cases) {
    for (const ColumnCodec codec : kIntCodecs) {
      CHECK(i64_roundtrips(c, codec));
    }
    CHECK(i64_roundtrips(c, ColumnCodec::DoubleSplit));
  }
}

static void test_f64_roundtrip() {
  std::vector<std::vector<double>> cases;
  cases.push_back({});
  cases.push_back({3.14159});
  cases.push_back(std::vector<double>(120, 2.5));
  {
    std::vector<double> ramp;
    for (int i = 0; i < 200; ++i) ramp.push_back(i * 0.25);
    cases.push_back(ramp);
  }
  {
    std::vector<double> rnd;
    uint64_t st = 7;
    for (int i = 0; i < 200; ++i) {
      uint64_t bits = splitmix(st);
      double d = 0.0;
      std::memcpy(&d, &bits, 8);
      if (d == d) rnd.push_back(d);
    }
    cases.push_back(rnd);
  }
  const double inf = std::numeric_limits<double>::infinity();
  const double qnan = std::numeric_limits<double>::quiet_NaN();
  cases.push_back({0.0, -0.0, inf, -inf, qnan, 1.0, -1.0,
                   std::numeric_limits<double>::denorm_min(),
                   std::numeric_limits<double>::max(),
                   std::numeric_limits<double>::lowest()});

  for (const auto& c : cases) CHECK(f64_roundtrips(c));

  std::vector<double> nanvec = {qnan, -qnan};
  const EncodedColumn enc = encode_f64_column(nanvec);
  std::vector<double> out;
  CHECK(decode_f64_column(enc, out));
  CHECK(out.size() == 2);
  CHECK(same_bits(out[0], qnan));
  CHECK(same_bits(out[1], -qnan));
}

static void test_best_codec() {
  std::vector<uint64_t> constant(500, 77);
  CHECK(best_codec_for(constant) == ColumnCodec::Rle);

  std::vector<uint64_t> ramp;
  for (uint64_t i = 0; i < 500; ++i) ramp.push_back(i);
  CHECK(best_codec_for(ramp) == ColumnCodec::DeltaZigzagVarint);

  std::vector<uint64_t> rnd;
  uint64_t st = 4242;
  for (int i = 0; i < 500; ++i) rnd.push_back(splitmix(st));
  CHECK(best_codec_for(rnd) == ColumnCodec::Raw);

  CHECK(best_codec_for({}) == ColumnCodec::Raw);
  CHECK(best_codec_for({5}) == ColumnCodec::DeltaZigzagVarint);
}

static void test_compression_ratio() {
  std::vector<uint64_t> constant(500, 77);
  const EncodedColumn rle = encode_u64_column(constant, ColumnCodec::Rle);
  CHECK(compression_ratio(rle) < 0.05);

  std::vector<uint64_t> ramp;
  for (uint64_t i = 0; i < 500; ++i) ramp.push_back(i);
  const EncodedColumn delta =
      encode_u64_column(ramp, ColumnCodec::DeltaZigzagVarint);
  CHECK(compression_ratio(delta) < 1.0);

  std::vector<uint64_t> rnd;
  uint64_t st = 88;
  for (int i = 0; i < 500; ++i) rnd.push_back(splitmix(st));
  const EncodedColumn raw = encode_u64_column(rnd, ColumnCodec::Raw);
  CHECK(compression_ratio(raw) >= 1.0);

  const EncodedColumn empty = encode_u64_column({}, ColumnCodec::Raw);
  CHECK(compression_ratio(empty) == 0.0);
}

static void test_decode_hardening_u64() {
  std::vector<uint64_t> sample;
  for (uint64_t i = 0; i < 40; ++i) sample.push_back(i * 7 + 3);

  for (const ColumnCodec codec : kIntCodecs) {
    const EncodedColumn good = encode_u64_column(sample, codec);
    std::vector<uint64_t> out;
    CHECK(decode_u64_column(good, out));

    EncodedColumn truncated = good;
    while (!truncated.bytes.empty()) {
      truncated.bytes.pop_back();
      std::vector<uint64_t> t;
      CHECK(!decode_u64_column(truncated, t));
    }

    EncodedColumn trailing = good;
    trailing.bytes.push_back(0x00);
    std::vector<uint64_t> tr;
    CHECK(!decode_u64_column(trailing, tr));

    EncodedColumn garbage = good;
    garbage.bytes.assign(6, 0xFF);
    std::vector<uint64_t> g;
    CHECK(!decode_u64_column(garbage, g));
  }

  EncodedColumn huge;
  huge.codec = ColumnCodec::Raw;
  huge.count = std::numeric_limits<size_t>::max();
  huge.bytes = {1, 2, 3};
  std::vector<uint64_t> ho;
  CHECK(!decode_u64_column(huge, ho));

  EncodedColumn short_raw;
  short_raw.codec = ColumnCodec::Raw;
  short_raw.count = 4;
  short_raw.bytes = {1, 2, 3};
  std::vector<uint64_t> so;
  CHECK(!decode_u64_column(short_raw, so));

  EncodedColumn empty_delta;
  empty_delta.codec = ColumnCodec::DeltaZigzagVarint;
  empty_delta.count = 3;
  std::vector<uint64_t> eo;
  CHECK(!decode_u64_column(empty_delta, eo));

  EncodedColumn rle_zero_run;
  rle_zero_run.codec = ColumnCodec::Rle;
  rle_zero_run.count = 3;
  rle_zero_run.bytes = {5, 0};
  std::vector<uint64_t> ro;
  CHECK(!decode_u64_column(rle_zero_run, ro));
}

static void test_decode_hardening_i64() {
  std::vector<int64_t> sample;
  for (int64_t i = 0; i < 40; ++i) sample.push_back(-i * 5 + 2);

  for (const ColumnCodec codec : kIntCodecs) {
    const EncodedColumn good = encode_i64_column(sample, codec);
    std::vector<int64_t> out;
    CHECK(decode_i64_column(good, out));

    EncodedColumn truncated = good;
    while (!truncated.bytes.empty()) {
      truncated.bytes.pop_back();
      std::vector<int64_t> t;
      CHECK(!decode_i64_column(truncated, t));
    }

    EncodedColumn garbage = good;
    garbage.bytes.assign(6, 0xFF);
    std::vector<int64_t> g;
    CHECK(!decode_i64_column(garbage, g));
  }

  EncodedColumn huge;
  huge.codec = ColumnCodec::DeltaZigzagVarint;
  huge.count = kMaxColumnCount + 1;
  huge.bytes = {1, 2, 3};
  std::vector<int64_t> ho;
  CHECK(!decode_i64_column(huge, ho));
}

static void test_decode_hardening_f64() {
  std::vector<double> sample;
  for (int i = 0; i < 40; ++i) sample.push_back(i * 1.5 - 3.0);
  const EncodedColumn good = encode_f64_column(sample);

  std::vector<double> out;
  CHECK(decode_f64_column(good, out));

  EncodedColumn truncated = good;
  while (!truncated.bytes.empty()) {
    truncated.bytes.pop_back();
    std::vector<double> t;
    CHECK(!decode_f64_column(truncated, t));
  }

  EncodedColumn garbage = good;
  garbage.bytes.assign(6, 0xFF);
  std::vector<double> g;
  CHECK(!decode_f64_column(garbage, g));

  EncodedColumn wrong_codec = good;
  wrong_codec.codec = ColumnCodec::Raw;
  std::vector<double> w;
  CHECK(!decode_f64_column(wrong_codec, w));
}

static void test_container_roundtrip() {
  std::vector<uint64_t> sample;
  for (uint64_t i = 0; i < 64; ++i) sample.push_back(i % 4);
  const EncodedColumn enc = encode_u64_column(sample, ColumnCodec::Rle);

  const std::vector<uint8_t> blob = serialize_column(enc);
  EncodedColumn back;
  CHECK(deserialize_column(blob.data(), blob.size(), back));
  CHECK(back.codec == enc.codec);
  CHECK(back.count == enc.count);
  CHECK(back.bytes == enc.bytes);

  std::vector<uint64_t> out;
  CHECK(decode_u64_column(back, out));
  CHECK(out == sample);

  for (size_t cut = 0; cut < blob.size(); ++cut) {
    EncodedColumn bad;
    CHECK(!deserialize_column(blob.data(), cut, bad));
  }

  std::vector<uint8_t> bad_magic = blob;
  bad_magic[0] = 'X';
  EncodedColumn bm;
  CHECK(!deserialize_column(bad_magic.data(), bad_magic.size(), bm));

  std::vector<uint8_t> trailing = blob;
  trailing.push_back(0xAB);
  EncodedColumn tb;
  CHECK(!deserialize_column(trailing.data(), trailing.size(), tb));

  EncodedColumn empty;
  CHECK(!deserialize_column(nullptr, 0, empty));
}

static ChannelSchema make_seq_schema(uint16_t channel_id) {
  ChannelSchema s;
  s.channel_id = channel_id;
  s.name = "counter";
  SchemaField f;
  f.name = "seq";
  f.type = FieldType::U32;
  s.fields.push_back(f);
  s.recompute_layout();
  return s;
}

static std::vector<uint8_t> build_block_payload(uint16_t channel_id,
                                                uint16_t version,
                                                uint16_t sample_count,
                                                const std::vector<uint8_t>& rows) {
  ByteWriter w;
  w.u16(channel_id);
  w.u16(version);
  w.u32(0);
  w.u16(sample_count);
  w.u16(0);
  w.bytes(rows);
  return w.buf();
}

static Frame make_block_frame(const std::vector<uint8_t>& payload) {
  Frame f;
  f.header.type = RecordType::SampleBlock;
  f.header.device_id = 0x1234u;
  f.header.monotonic_ts = 1000;
  f.payload = payload;
  return f;
}

static void test_adapter_over_store() {
  SchemaRegistry reg;
  const ChannelSchema* v1 = reg.register_schema(make_seq_schema(9));
  CHECK(v1 != nullptr && v1->stride == 4);

  ByteWriter rows;
  const int n = 128;
  for (int i = 0; i < n; ++i) rows.u32(555u);

  ChannelStore store;
  const Frame f = make_block_frame(
      build_block_payload(9, 1, static_cast<uint16_t>(n), rows.buf()));
  CHECK(store.ingest_frame(f, reg) == ChannelError::None);
  CHECK(store.total_samples() == static_cast<size_t>(n));

  const ColumnStats s = compress_channel_field(store, 9, 0);
  CHECK(s.raw_bytes == static_cast<size_t>(n) * 8);
  CHECK(s.encoded_bytes > 0);
  CHECK(s.encoded_bytes < s.raw_bytes);
  CHECK(s.codec == ColumnCodec::Rle);
  CHECK(s.ratio < 1.0);
  CHECK(s.ratio > 0.0);

  const std::string desc = describe_column_stats(s);
  CHECK(!desc.empty());
  CHECK(desc.find("codec=Rle") != std::string::npos);
  CHECK(desc.find("ratio=") != std::string::npos);

  const ColumnStats missing = compress_channel_field(store, 404, 0);
  CHECK(missing.raw_bytes == 0);
  CHECK(missing.encoded_bytes == 0);
  CHECK(missing.ratio == 0.0);
}

static void test_ramp_field_adapter() {
  SchemaRegistry reg;
  reg.register_schema(make_seq_schema(11));

  ByteWriter rows;
  const int n = 200;
  for (int i = 0; i < n; ++i) rows.u32(static_cast<uint32_t>(1000 + i));

  ChannelStore store;
  const Frame f = make_block_frame(
      build_block_payload(11, 1, static_cast<uint16_t>(n), rows.buf()));
  CHECK(store.ingest_frame(f, reg) == ChannelError::None);

  const ColumnStats s = compress_channel_field(store, 11, 0);
  CHECK(s.codec == ColumnCodec::DeltaZigzagVarint);
  CHECK(s.ratio < 1.0);
}

static void test_codec_names() {
  CHECK(std::strcmp(column_codec_name(ColumnCodec::Raw), "Raw") == 0);
  CHECK(std::strcmp(column_codec_name(ColumnCodec::DeltaZigzagVarint),
                    "DeltaZigzagVarint") == 0);
  CHECK(std::strcmp(column_codec_name(ColumnCodec::Rle), "Rle") == 0);
  CHECK(std::strcmp(column_codec_name(ColumnCodec::DoubleSplit),
                    "DoubleSplit") == 0);
}

int main() {
  test_codec_names();
  test_u64_roundtrip();
  test_i64_roundtrip();
  test_f64_roundtrip();
  test_best_codec();
  test_compression_ratio();
  test_decode_hardening_u64();
  test_decode_hardening_i64();
  test_decode_hardening_f64();
  test_container_roundtrip();
  test_adapter_over_store();
  test_ramp_field_adapter();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all compress tests passed\n");
  return 0;
}
