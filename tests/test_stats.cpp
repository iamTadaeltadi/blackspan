#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/byteio.h"
#include "blackspan/channel.h"
#include "blackspan/frame.h"
#include "blackspan/schema.h"
#include "blackspan/stats.h"

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

static bool close_to(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  return std::fabs(a - b) < 1e-9;
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

// Builds a schema registry + channel store holding channel 1 with one u16 field
// "raw" and samples {100, 200, 300}.
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
    p.u8(3);  // U16
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
  p.u8(0);  // no detail
  put_frame(wire, RecordType::Fault, 0x100, seq, ts, p.buf());
}

}  // namespace

static void test_compute_stats() {
  ColumnStats s = compute_stats({1, 2, 3, 4});
  CHECK(s.valid);
  CHECK(s.count == 4);
  CHECK(close_to(s.sum, 10.0));
  CHECK(close_to(s.min, 1.0));
  CHECK(close_to(s.max, 4.0));
  CHECK(close_to(s.mean, 2.5));
  CHECK(close_to(s.stddev, std::sqrt(1.25)));

  ColumnStats e = compute_stats({});
  CHECK(!e.valid);
  CHECK(e.count == 0);
}

static void test_percentile() {
  std::vector<double> v = {1, 2, 3, 4};
  CHECK(close_to(percentile(v, 0), 1.0));
  CHECK(close_to(percentile(v, 100), 4.0));
  CHECK(close_to(percentile(v, 50), 2.5));
  CHECK(close_to(percentile(v, 25), 1.75));
  CHECK(close_to(median(v), 2.5));
  CHECK(std::isnan(percentile({}, 50)));
}

static void test_histogram() {
  Histogram h = histogram({1, 2, 3, 4}, 2, 0.0, 4.0);
  CHECK(close_to(h.bucket_width, 2.0));
  CHECK(h.buckets.size() == 2);
  CHECK(h.buckets[0] == 1);  // [0,2): {1}
  CHECK(h.buckets[1] == 3);  // [2,4]: {2,3,4}
  CHECK(h.below == 0 && h.above == 0);

  Histogram h2 = histogram({-1, 5}, 2, 0.0, 4.0);
  CHECK(h2.below == 1 && h2.above == 1);
  CHECK(h2.buckets[0] == 0 && h2.buckets[1] == 0);
}

static void test_downsample_and_rate() {
  std::vector<double> d = downsample_mean({1, 2, 3, 4, 5}, 2);
  CHECK(d.size() == 3);
  CHECK(close_to(d[0], 1.5) && close_to(d[1], 3.5) && close_to(d[2], 5.0));
  CHECK(downsample_mean({}, 2).empty());
  CHECK(downsample_mean({1, 2}, 0).empty());

  std::vector<double> out;
  CHECK(rate_of_change({0, 10, 20}, {0, 1, 2}, out) == StatsError::None);
  CHECK(out.size() == 2 && close_to(out[0], 10.0) && close_to(out[1], 10.0));

  std::vector<double> out2;
  CHECK(rate_of_change({0, 5}, {7, 7}, out2) == StatsError::None);
  CHECK(out2.size() == 1 && std::isnan(out2[0]));

  std::vector<double> out3;
  CHECK(rate_of_change({0, 1, 2}, {0, 1}, out3) == StatsError::LengthMismatch);
  CHECK(out3.empty());
}

static void test_column_stats() {
  SchemaRegistry schemas;
  ChannelStore channels;
  build_channels(schemas, channels);

  ColumnStats out;
  CHECK(compute_column_stats(channels, 1, 0, out) == StatsError::None);
  CHECK(out.valid && out.count == 3);
  CHECK(close_to(out.min, 100.0) && close_to(out.max, 300.0));
  CHECK(close_to(out.mean, 200.0));

  ColumnStats out2;
  CHECK(compute_column_stats(channels, 99, 0, out2) == StatsError::UnknownChannel);
  ColumnStats out3;
  CHECK(compute_column_stats(channels, 1, 5, out3) == StatsError::BadField);
}

static void test_event_stats() {
  std::vector<uint8_t> wire;
  add_fault(wire, 0, 1000, 10, 0);  // Info
  add_fault(wire, 1, 1100, 11, 1);  // Warning
  add_fault(wire, 2, 1200, 12, 2);  // Error
  add_fault(wire, 3, 1300, 13, 3);  // Critical
  AnomalyEngine engine;
  engine.ingest(wire.data(), wire.size());
  CHECK(engine.faults().size() == 4);

  std::array<size_t, 4> sev = severity_histogram(engine);
  CHECK(sev[0] == 1 && sev[1] == 1 && sev[2] == 1 && sev[3] == 1);

  CHECK(close_to(mean_interfault_ns(engine), 100.0));  // gaps 100,100,100

  // span 300 ns, 4 faults, window 100 -> 4/300*100
  CHECK(close_to(fault_rate(engine, 100), 4.0 / 300.0 * 100.0));

  AnomalyEngine empty;
  CHECK(std::isnan(mean_interfault_ns(empty)));
  CHECK(close_to(fault_rate(empty, 100), 0.0));
}

int main() {
  test_compute_stats();
  test_percentile();
  test_histogram();
  test_downsample_and_rate();
  test_column_stats();
  test_event_stats();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all stats tests passed\n");
  return 0;
}
