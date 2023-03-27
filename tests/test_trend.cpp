#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/byteio.h"
#include "blackspan/channel.h"
#include "blackspan/frame.h"
#include "blackspan/schema.h"
#include "blackspan/trend.h"

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

static bool near_eps(double a, double b, double eps) {
  return std::fabs(a - b) < eps;
}

namespace {

std::vector<std::pair<uint64_t, double>> make_series(
    const std::vector<double>& values, uint64_t start, uint64_t step) {
  std::vector<std::pair<uint64_t, double>> out;
  out.reserve(values.size());
  uint64_t ts = start;
  for (const double v : values) {
    out.emplace_back(ts, v);
    ts += step;
  }
  return out;
}

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
                const std::string& channel_name, const std::string& field_name) {
  ByteWriter p;
  p.u16(channel_id);
  p.u8(1);
  p.u8(1);
  p.u8(static_cast<uint8_t>(channel_name.size()));
  p.bytes(reinterpret_cast<const uint8_t*>(channel_name.data()),
          channel_name.size());
  p.u8(3);
  p.u8(static_cast<uint8_t>(field_name.size()));
  p.bytes(reinterpret_cast<const uint8_t*>(field_name.data()),
          field_name.size());
  put_frame(wire, RecordType::SchemaDef, 0x200, 0, 500, p.buf());
}

void put_sample(std::vector<uint8_t>& wire, uint16_t channel_id, uint32_t seq,
                uint64_t ts, uint16_t value) {
  ByteWriter p;
  p.u16(channel_id);
  p.u16(1);
  p.u32(0);
  p.u16(1);
  p.u16(0);
  p.u16(value);
  put_frame(wire, RecordType::SampleBlock, 0x200, seq, ts, p.buf());
}

void build_store(SchemaRegistry& schemas, ChannelStore& channels) {
  std::vector<uint8_t> wire;
  put_schema(wire, 1, "ramp", "raw");
  put_schema(wire, 2, "flat", "raw");
  uint32_t seq = 1;
  for (int i = 0; i < 10; ++i) {
    put_sample(wire, 1, seq++, 2000 + static_cast<uint64_t>(i) * 100,
               static_cast<uint16_t>(100 + i * 50));
  }
  for (int i = 0; i < 10; ++i) {
    put_sample(wire, 2, seq++, 2000 + static_cast<uint64_t>(i) * 100, 200);
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

static void test_fit_trend() {
  std::vector<double> ramp;
  for (int i = 0; i < 10; ++i) ramp.push_back(5.0 + 3.0 * (i * 10));
  TrendLine line = fit_trend(make_series(ramp, 1000, 10));
  CHECK(near_eps(line.slope, 3.0, 1e-6));
  CHECK(near_eps(line.intercept, 5.0, 1e-6));
  CHECK(near_eps(line.r2, 1.0, 1e-9));
  CHECK(line.t0 == 1000);

  std::vector<double> falling;
  for (int i = 0; i < 8; ++i) falling.push_back(100.0 - 2.0 * i);
  TrendLine down = fit_trend(make_series(falling, 0, 5));
  CHECK(down.slope < 0.0);
  CHECK(near_eps(down.r2, 1.0, 1e-9));

  TrendLine flat = fit_trend(make_series({7, 7, 7, 7, 7}, 0, 1));
  CHECK(near_eps(flat.slope, 0.0, 1e-12));
  CHECK(near_eps(flat.r2, 0.0, 1e-12));

  TrendLine empty = fit_trend({});
  CHECK(near_eps(empty.slope, 0.0, 1e-12) && near_eps(empty.r2, 0.0, 1e-12));
  CHECK(empty.t0 == 0);

  std::vector<std::pair<uint64_t, double>> single = {{42, 9.0}};
  TrendLine one = fit_trend(single);
  CHECK(near_eps(one.slope, 0.0, 1e-12));
  CHECK(near_eps(one.intercept, 9.0, 1e-12));
  CHECK(one.t0 == 42);

  std::vector<std::pair<uint64_t, double>> same_ts = {{5, 1.0}, {5, 4.0}};
  TrendLine deg = fit_trend(same_ts);
  CHECK(near_eps(deg.slope, 0.0, 1e-12));
  CHECK(near_eps(deg.r2, 0.0, 1e-12));
}

static void test_segments_step() {
  std::vector<double> stepped;
  for (int i = 0; i < 10; ++i) stepped.push_back(0.0);
  for (int i = 0; i < 10; ++i) stepped.push_back(100.0);
  std::vector<Segment> segs =
      segment_by_changepoints(make_series(stepped, 0, 1), 20.0, 3);
  CHECK(segs.size() == 2);
  if (segs.size() == 2) {
    CHECK(segs[0].begin_index == 0 && segs[0].end_index == 10);
    CHECK(segs[1].begin_index == 10 && segs[1].end_index == 20);
    CHECK(near_eps(segs[0].mean, 0.0, 1e-9));
    CHECK(near_eps(segs[1].mean, 100.0, 1e-9));
  }
}

static void test_segments_ramp() {
  std::vector<double> ramp;
  for (int i = 0; i < 20; ++i) ramp.push_back(static_cast<double>(i));
  std::vector<Segment> segs =
      segment_by_changepoints(make_series(ramp, 0, 1), 50.0, 3);
  CHECK(segs.size() == 1);
  if (segs.size() == 1) {
    CHECK(segs[0].begin_index == 0 && segs[0].end_index == 20);
    CHECK(near_eps(segs[0].slope, 1.0, 1e-9));
  }
}

static void test_segments_blip_suppressed() {
  std::vector<double> data;
  for (int i = 0; i < 8; ++i) data.push_back(0.0);
  data.push_back(40.0);
  for (int i = 0; i < 7; ++i) data.push_back(0.0);
  for (int i = 0; i < 8; ++i) data.push_back(100.0);
  std::vector<Segment> segs =
      segment_by_changepoints(make_series(data, 0, 1), 25.0, 3);
  CHECK(segs.size() == 2);
  if (segs.size() == 2) {
    CHECK(segs[0].begin_index == 0);
    CHECK(segs[0].end_index == 16);
    CHECK(segs[1].begin_index == 16);
    CHECK(segs[1].end_index == 24);
  }
}

static void test_segments_degenerate() {
  CHECK(segment_by_changepoints({}, 1.0, 3).empty());

  std::vector<Segment> one =
      segment_by_changepoints(make_series({9.0}, 7, 1), 1.0, 3);
  CHECK(one.size() == 1);
  if (one.size() == 1) {
    CHECK(one[0].begin_index == 0 && one[0].end_index == 1);
    CHECK(near_eps(one[0].mean, 9.0, 1e-12));
    CHECK(near_eps(one[0].slope, 0.0, 1e-12));
  }

  std::vector<Segment> constant =
      segment_by_changepoints(make_series({4, 4, 4, 4, 4}, 0, 1), 1.0, 2);
  CHECK(constant.size() == 1);
  if (constant.size() == 1) {
    CHECK(constant[0].begin_index == 0 && constant[0].end_index == 5);
  }
}

static void test_monotonic_runs() {
  std::vector<MonotonicRun> runs =
      monotonic_runs(make_series({1, 3, 5, 4, 2, 0}, 0, 1));
  CHECK(runs.size() == 2);
  if (runs.size() == 2) {
    CHECK(runs[0].begin_index == 0 && runs[0].end_index == 2);
    CHECK(runs[0].increasing);
    CHECK(runs[1].begin_index == 2 && runs[1].end_index == 5);
    CHECK(!runs[1].increasing);
  }

  std::vector<MonotonicRun> flat = monotonic_runs(make_series({5, 5, 5}, 0, 1));
  CHECK(flat.size() == 1);
  if (flat.size() == 1) {
    CHECK(flat[0].begin_index == 0 && flat[0].end_index == 2);
    CHECK(flat[0].increasing);
  }

  std::vector<MonotonicRun> plateau =
      monotonic_runs(make_series({1, 2, 2, 3}, 0, 1));
  CHECK(plateau.size() == 1);
  if (plateau.size() == 1) CHECK(plateau[0].increasing);

  std::vector<MonotonicRun> alt =
      monotonic_runs(make_series({1, 2, 1, 2}, 0, 1));
  CHECK(alt.size() == 3);

  CHECK(monotonic_runs({}).empty());
  CHECK(monotonic_runs(make_series({9}, 0, 1)).empty());
}

static void test_drift_rate() {
  std::vector<double> rising;
  for (int i = 0; i < 6; ++i) rising.push_back(2.0 * i);
  CHECK(drift_rate(make_series(rising, 100, 1)) > 0.0);

  std::vector<double> falling;
  for (int i = 0; i < 6; ++i) falling.push_back(50.0 - 3.0 * i);
  CHECK(drift_rate(make_series(falling, 100, 1)) < 0.0);

  CHECK(near_eps(drift_rate(make_series({8, 8, 8}, 0, 1)), 0.0, 1e-12));
  CHECK(near_eps(drift_rate({}), 0.0, 1e-12));
}

static void test_adapter() {
  SchemaRegistry schemas;
  ChannelStore channels;
  build_store(schemas, channels);

  std::vector<TrendSummary> summaries = analyze_trends(channels);
  CHECK(summaries.size() == 2);

  bool saw_ramp = false;
  bool saw_flat = false;
  for (const TrendSummary& s : summaries) {
    if (s.channel_id == 1) {
      saw_ramp = true;
      CHECK(s.field_index == 0);
      CHECK(s.samples == 10);
      CHECK(s.slope > 0.0);
      CHECK(s.drift_rate > 0.0);
      CHECK(s.r2 > 0.99);
      CHECK(s.monotonic_runs == 1);
      CHECK(s.changepoints == 0);
      CHECK(describe_trend(s).find("rising") != std::string::npos);
    }
    if (s.channel_id == 2) {
      saw_flat = true;
      CHECK(s.samples == 10);
      CHECK(near_eps(s.slope, 0.0, 1e-9));
      CHECK(describe_trend(s).find("flat") != std::string::npos);
    }
  }
  CHECK(saw_ramp && saw_flat);

  ChannelStore empty;
  CHECK(analyze_trends(empty).empty());
}

int main() {
  test_fit_trend();
  test_segments_step();
  test_segments_ramp();
  test_segments_blip_suppressed();
  test_segments_degenerate();
  test_monotonic_runs();
  test_drift_rate();
  test_adapter();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_trend: all checks passed\n");
  return 0;
}
