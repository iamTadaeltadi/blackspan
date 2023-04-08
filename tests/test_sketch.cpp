#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "blackspan/byteio.h"
#include "blackspan/channel.h"
#include "blackspan/frame.h"
#include "blackspan/schema.h"
#include "blackspan/sketch.h"

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

static bool close_to(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  return std::fabs(a - b) < 1e-9;
}
static bool near_eps(double a, double b, double eps) {
  return std::fabs(a - b) < eps;
}

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

void build_ramp_channel(SchemaRegistry& schemas, ChannelStore& channels,
                        uint16_t count) {
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
    p.u16(count);
    p.u16(0);
    for (uint16_t i = 0; i < count; ++i) p.u16(i);
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

}

static void test_reservoir_keeps_all() {
  ReservoirSampler rs(10, 42);
  for (int i = 0; i < 5; ++i) rs.push(static_cast<double>(i));
  CHECK(rs.seen() == 5);
  CHECK(rs.samples().size() == 5);
  CHECK(close_to(rs.approx_quantile(0.0), 0.0));
  CHECK(close_to(rs.approx_quantile(1.0), 4.0));
  CHECK(close_to(rs.approx_quantile(0.5), 2.0));
  CHECK(close_to(rs.approx_quantile(0.25), 1.0));
}

static void test_reservoir_subsample() {
  ReservoirSampler rs(4, 7);
  for (int i = 0; i < 100; ++i) rs.push(static_cast<double>(i));
  CHECK(rs.seen() == 100);
  CHECK(rs.samples().size() == 4);
  for (double v : rs.samples()) CHECK(v >= 0.0 && v <= 99.0);

  double q = rs.approx_quantile(0.5);
  CHECK(!std::isnan(q));
}

static void test_reservoir_deterministic() {
  ReservoirSampler a(8, 12345);
  ReservoirSampler b(8, 12345);
  for (int i = 0; i < 500; ++i) {
    a.push(static_cast<double>(i * 3 % 251));
    b.push(static_cast<double>(i * 3 % 251));
  }
  CHECK(a.samples().size() == b.samples().size());
  bool identical = a.samples().size() == b.samples().size();
  for (size_t i = 0; i < a.samples().size() && identical; ++i) {
    if (!close_to(a.samples()[i], b.samples()[i])) identical = false;
  }
  CHECK(identical);

  ReservoirSampler c(8, 999);
  for (int i = 0; i < 500; ++i) c.push(static_cast<double>(i * 3 % 251));
  bool differs = false;
  for (size_t i = 0; i < c.samples().size(); ++i) {
    if (!close_to(c.samples()[i], a.samples()[i])) differs = true;
  }
  CHECK(differs);
}

static void test_reservoir_degenerate() {
  ReservoirSampler zero(0, 5);
  for (int i = 0; i < 10; ++i) zero.push(static_cast<double>(i));
  CHECK(zero.samples().empty());
  CHECK(zero.seen() == 10);
  CHECK(std::isnan(zero.approx_quantile(0.5)));

  ReservoirSampler empty(4, 5);
  CHECK(empty.seen() == 0);
  CHECK(std::isnan(empty.approx_quantile(0.5)));

  ReservoirSampler single(5, 5);
  single.push(42.0);
  CHECK(single.samples().size() == 1);
  CHECK(single.seen() == 1);
  CHECK(close_to(single.approx_quantile(0.0), 42.0));
  CHECK(close_to(single.approx_quantile(0.5), 42.0));
  CHECK(close_to(single.approx_quantile(1.0), 42.0));
}

static void test_moments() {
  const std::vector<double> v = {2, 4, 4, 4, 5, 5, 7, 9};
  Moments m = streaming_moments(v);
  CHECK(m.n == 8);
  CHECK(close_to(m.mean, 5.0));
  CHECK(close_to(m.variance(), 4.0));
  CHECK(close_to(m.stddev(), 2.0));

  double direct_mean = 0.0;
  for (double x : v) direct_mean += x;
  direct_mean /= static_cast<double>(v.size());
  double direct_var = 0.0;
  for (double x : v) direct_var += (x - direct_mean) * (x - direct_mean);
  direct_var /= static_cast<double>(v.size());
  CHECK(near_eps(m.mean, direct_mean, 1e-12));
  CHECK(near_eps(m.variance(), direct_var, 1e-12));

  Moments e = streaming_moments({});
  CHECK(e.n == 0);
  CHECK(close_to(e.mean, 0.0));
  CHECK(close_to(e.variance(), 0.0));
  CHECK(close_to(e.stddev(), 0.0));

  Moments s = streaming_moments({7.5});
  CHECK(s.n == 1);
  CHECK(close_to(s.mean, 7.5));
  CHECK(close_to(s.variance(), 0.0));
  CHECK(close_to(s.stddev(), 0.0));
}

static void test_combine_moments() {
  std::vector<double> whole;
  for (int i = 0; i < 40; ++i) whole.push_back(static_cast<double>(i * i % 17));
  std::vector<double> left(whole.begin(), whole.begin() + 13);
  std::vector<double> right(whole.begin() + 13, whole.end());

  Moments full = streaming_moments(whole);
  Moments merged =
      combine_moments(streaming_moments(left), streaming_moments(right));
  CHECK(merged.n == full.n);
  CHECK(near_eps(merged.mean, full.mean, 1e-9));
  CHECK(near_eps(merged.variance(), full.variance(), 1e-9));

  Moments with_empty = combine_moments(full, streaming_moments({}));
  CHECK(with_empty.n == full.n);
  CHECK(near_eps(with_empty.mean, full.mean, 1e-12));
  Moments empty_with = combine_moments(streaming_moments({}), full);
  CHECK(near_eps(empty_with.variance(), full.variance(), 1e-12));
}

static void test_tdigest_merge() {
  TDigestLite a(100);
  TDigestLite b(100);
  for (int i = 0; i < 500; ++i) a.push(static_cast<double>(i));
  for (int i = 500; i < 1000; ++i) b.push(static_cast<double>(i));
  a.merge(b);
  CHECK(a.count() == 1000);
  CHECK(near_eps(a.quantile(0.5), 500.0, 30.0));
  CHECK(near_eps(a.quantile(0.9), 900.0, 30.0));

  TDigestLite c(50);
  c.merge(TDigestLite(50));
  CHECK(c.count() == 0);
  CHECK(std::isnan(c.quantile(0.5)));
}

static void test_countmin_merge() {
  CountMinSketch a(64, 4, 77);
  CountMinSketch b(64, 4, 77);
  a.add(1, 5);
  a.add(2, 2);
  b.add(1, 3);
  b.add(9, 4);
  CHECK(a.merge(b));
  CHECK(a.estimate(1) >= 8);
  CHECK(a.total() == 14);

  CountMinSketch mismatch(32, 4, 77);
  CHECK(!a.merge(mismatch));
  CountMinSketch other_seed(64, 4, 78);
  CHECK(!a.merge(other_seed));
}

static void test_tdigest_ramp() {
  TDigestLite d(100);
  for (int i = 0; i < 1000; ++i) d.push(static_cast<double>(i));
  CHECK(d.count() == 1000);

  CHECK(near_eps(d.quantile(0.5), 500.0, 25.0));
  CHECK(near_eps(d.quantile(0.9), 900.0, 25.0));
  CHECK(near_eps(d.quantile(0.99), 990.0, 25.0));
  CHECK(near_eps(d.quantile(0.0), 0.0, 5.0));
  CHECK(near_eps(d.quantile(1.0), 999.0, 5.0));

  double prev = -1e18;
  bool monotonic = true;
  for (int i = -100; i <= 1100; ++i) {
    double c = d.cdf(static_cast<double>(i));
    if (c + 1e-12 < prev) monotonic = false;
    prev = c;
    CHECK(c >= 0.0 && c <= 1.0);
  }
  CHECK(monotonic);
  CHECK(close_to(d.cdf(-100.0), 0.0));
  CHECK(close_to(d.cdf(2000.0), 1.0));
  CHECK(near_eps(d.cdf(500.0), 0.5, 0.05));
}

static void test_tdigest_degenerate() {
  TDigestLite empty(50);
  CHECK(empty.count() == 0);
  CHECK(std::isnan(empty.quantile(0.5)));
  CHECK(close_to(empty.cdf(1.0), 0.0));

  TDigestLite single(10);
  single.push(42.0);
  CHECK(single.count() == 1);
  CHECK(close_to(single.quantile(0.0), 42.0));
  CHECK(close_to(single.quantile(0.5), 42.0));
  CHECK(close_to(single.quantile(1.0), 42.0));
  CHECK(close_to(single.cdf(41.0), 0.0));
  CHECK(close_to(single.cdf(43.0), 1.0));

  TDigestLite zero(0);
  for (int i = 0; i < 200; ++i) zero.push(static_cast<double>(i));
  CHECK(zero.count() == 200);
  double q = zero.quantile(0.5);
  CHECK(!std::isnan(q));
  CHECK(q >= 0.0 && q <= 199.0);
}

static void test_countmin_exact() {
  CountMinSketch cm(4096, 5, 20260728);
  cm.add(11, 3);
  cm.add(22, 1);
  cm.add(33, 7);
  cm.add(44, 2);
  CHECK(cm.estimate(11) == 3);
  CHECK(cm.estimate(22) == 1);
  CHECK(cm.estimate(33) == 7);
  CHECK(cm.estimate(44) == 2);
  CHECK(cm.total() == 13);

  CountMinSketch acc(2048, 4, 1);
  acc.add(100, 1);
  acc.add(100, 1);
  acc.add(100, 1);
  CHECK(acc.estimate(100) == 3);
  CHECK(acc.total() == 3);
}

static void test_countmin_overestimate() {
  CountMinSketch cm(16, 3, 42);
  std::map<uint64_t, uint64_t> truth;
  uint64_t total = 0;
  for (uint64_t k = 0; k < 300; ++k) {
    uint64_t c = (k % 5) + 1;
    cm.add(k, c);
    truth[k] += c;
    total += c;
  }
  bool never_under = true;
  for (const auto& kv : truth) {
    if (cm.estimate(kv.first) < kv.second) never_under = false;
  }
  CHECK(never_under);
  CHECK(cm.total() == total);
}

static void test_countmin_degenerate() {
  CountMinSketch zero_w(0, 4, 1);
  zero_w.add(5, 3);
  CHECK(zero_w.estimate(5) == 0);
  CHECK(zero_w.total() == 3);

  CountMinSketch zero_d(16, 0, 1);
  zero_d.add(5, 3);
  CHECK(zero_d.estimate(5) == 0);
  CHECK(zero_d.total() == 3);

  CountMinSketch both(0, 0, 1);
  both.add(9, 4);
  CHECK(both.estimate(9) == 0);
  CHECK(both.total() == 4);
}

static void test_adapter() {
  SchemaRegistry schemas;
  ChannelStore channels;
  build_ramp_channel(schemas, channels, 100);

  std::vector<FieldSketch> sk = sketch_channels(channels, 1000, 42);
  CHECK(sk.size() == 1);
  if (sk.size() == 1) {
    const FieldSketch& s = sk[0];
    CHECK(s.channel_id == 1);
    CHECK(s.field_index == 0);
    CHECK(s.samples == 100);
    CHECK(near_eps(s.p50, 49.5, 1e-6));
    CHECK(near_eps(s.p90, 89.1, 1e-6));
    CHECK(near_eps(s.p99, 98.01, 1e-6));
    CHECK(near_eps(s.mean, 49.5, 1e-9));
    CHECK(s.stddev > 0.0);

    std::string desc = describe_sketch(s);
    CHECK(!desc.empty());
    CHECK(desc.find("ch=1") != std::string::npos);
  }

  std::vector<FieldSketch> small = sketch_channels(channels, 16, 7);
  CHECK(small.size() == 1);
  if (small.size() == 1) {
    CHECK(small[0].samples == 100);
    CHECK(small[0].p50 >= 20.0 && small[0].p50 <= 80.0);
    CHECK(near_eps(small[0].mean, 49.5, 1e-9));
  }

  ChannelStore empty;
  CHECK(sketch_channels(empty, 100, 1).empty());
}

int main() {
  test_reservoir_keeps_all();
  test_reservoir_subsample();
  test_reservoir_deterministic();
  test_reservoir_degenerate();
  test_moments();
  test_combine_moments();
  test_tdigest_merge();
  test_countmin_merge();
  test_tdigest_ramp();
  test_tdigest_degenerate();
  test_countmin_exact();
  test_countmin_overestimate();
  test_countmin_degenerate();
  test_adapter();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_sketch: all checks passed\n");
  return 0;
}
