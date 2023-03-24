#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/byteio.h"
#include "blackspan/channel.h"
#include "blackspan/frame.h"
#include "blackspan/schema.h"
#include "blackspan/stats_advanced.h"

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

}

static void test_percentile() {
  const std::vector<double> v = {1, 2, 3, 4};

  CHECK(close_to(percentile(v, 0, PercentileMethod::Linear), 1.0));
  CHECK(close_to(percentile(v, 100, PercentileMethod::Linear), 4.0));
  CHECK(close_to(percentile(v, 25, PercentileMethod::Linear), 1.75));
  CHECK(close_to(percentile(v, 50, PercentileMethod::Linear), 2.5));
  CHECK(close_to(percentile(v, 75, PercentileMethod::Linear), 3.25));

  CHECK(close_to(percentile(v, 0, PercentileMethod::NearestRank), 1.0));
  CHECK(close_to(percentile(v, 25, PercentileMethod::NearestRank), 1.0));
  CHECK(close_to(percentile(v, 50, PercentileMethod::NearestRank), 2.0));
  CHECK(close_to(percentile(v, 75, PercentileMethod::NearestRank), 3.0));
  CHECK(close_to(percentile(v, 100, PercentileMethod::NearestRank), 4.0));

  const std::vector<double> w = {1, 2, 3, 4, 5};
  CHECK(close_to(percentile(w, 25, PercentileMethod::NearestRank), 2.0));
  CHECK(close_to(percentile(w, 50, PercentileMethod::NearestRank), 3.0));

  CHECK(close_to(percentile(v, -10, PercentileMethod::Linear), 1.0));
  CHECK(close_to(percentile(v, 110, PercentileMethod::Linear), 4.0));
  CHECK(v[0] == 1 && v[3] == 4);

  CHECK(close_to(median(v), 2.5));
  CHECK(close_to(quantile_iqr(v), 1.5));

  CHECK(std::isnan(percentile({}, 50, PercentileMethod::Linear)));
  CHECK(std::isnan(percentile({}, 50, PercentileMethod::NearestRank)));
  CHECK(std::isnan(median({})));
  CHECK(std::isnan(quantile_iqr({})));
  CHECK(std::isnan(percentile(v, std::nan(""), PercentileMethod::Linear)));
  CHECK(close_to(percentile({42}, 30, PercentileMethod::Linear), 42.0));
  CHECK(close_to(percentile({42}, 30, PercentileMethod::NearestRank), 42.0));
  CHECK(close_to(quantile_iqr({7}), 0.0));
}

static void test_moments() {

  const std::vector<double> v = {2, 4, 4, 4, 5, 5, 7, 9};
  CHECK(close_to(variance(v, false), 4.0));
  CHECK(close_to(stddev(v, false), 2.0));
  CHECK(near_eps(variance(v, true), 32.0 / 7.0, 1e-9));
  CHECK(near_eps(stddev(v, true), std::sqrt(32.0 / 7.0), 1e-9));

  const std::vector<double> sk = {1, 1, 1, 2, 2, 3, 4, 10};
  CHECK(skewness(sk) > 0.0);
  CHECK(near_eps(skewness(sk), 1.7567184095, 1e-6));
  CHECK(kurtosis_excess(sk) > 0.0);
  CHECK(near_eps(kurtosis_excess(sk), 1.7890625, 1e-6));

  const std::vector<double> ls = {-10, -4, -3, -2, -2, -1, -1, -1};
  CHECK(skewness(ls) < 0.0);

  CHECK(close_to(variance({}, true), 0.0));
  CHECK(close_to(variance({5}, true), 0.0));
  CHECK(close_to(variance({5}, false), 0.0));
  CHECK(close_to(stddev({}, false), 0.0));
  CHECK(close_to(skewness({1, 2}), 0.0));
  CHECK(close_to(skewness({7, 7, 7, 7}), 0.0));
  CHECK(close_to(kurtosis_excess({1}), 0.0));
  CHECK(close_to(kurtosis_excess({7, 7, 7, 7}), 0.0));
}

static void test_histogram() {

  const std::vector<double> v = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  Histogram h = histogram(v, 5);
  CHECK(h.counts.size() == 5);
  CHECK(close_to(h.lo, 0.0) && close_to(h.hi, 10.0));
  CHECK(close_to(h.width, 2.0));
  CHECK(h.counts[0] == 2 && h.counts[1] == 2 && h.counts[2] == 2 &&
        h.counts[3] == 2 && h.counts[4] == 3);
  size_t total = 0;
  for (size_t c : h.counts) total += c;
  CHECK(total == v.size());

  Histogram h2 = histogram({1, 2, 3, 4}, 2);
  CHECK(close_to(h2.width, 1.5));
  CHECK(h2.counts[0] == 2 && h2.counts[1] == 2);

  Histogram h3 = histogram({5, 5, 5}, 3);
  CHECK(close_to(h3.width, 0.0));
  CHECK(h3.counts.size() == 3);
  CHECK(h3.counts[0] == 3 && h3.counts[1] == 0 && h3.counts[2] == 0);

  Histogram h4 = histogram({}, 4);
  CHECK(h4.counts.size() == 4 && h4.counts[0] == 0);
  Histogram h5 = histogram(v, 0);
  CHECK(h5.counts.empty());
}

static void test_ewma() {

  std::vector<double> out = ewma({1, 2, 3, 4}, 0.5);
  CHECK(out.size() == 4);
  CHECK(close_to(out[0], 1.0));
  CHECK(close_to(out[1], 1.5));
  CHECK(close_to(out[2], 2.25));
  CHECK(close_to(out[3], 3.125));

  std::vector<double> one = ewma({3, 1, 4}, 1.0);
  CHECK(close_to(one[0], 3.0) && close_to(one[1], 1.0) && close_to(one[2], 4.0));

  CHECK(ewma({}, 0.5).empty());
  std::vector<double> single = ewma({9}, 0.3);
  CHECK(single.size() == 1 && close_to(single[0], 9.0));
}

static void test_linear_fit() {

  const std::vector<double> x = {0, 1, 2, 3, 4};
  const std::vector<double> y = {1, 3, 5, 7, 9};
  LinearFit f;
  CHECK(linear_fit(x, y, f));
  CHECK(close_to(f.slope, 2.0));
  CHECK(close_to(f.intercept, 1.0));
  CHECK(near_eps(f.r2, 1.0, 1e-12));

  const std::vector<double> yn = {1.1, 2.9, 5.2, 6.8, 9.1};
  LinearFit g;
  CHECK(linear_fit(x, yn, g));
  CHECK(g.slope > 0.0);
  CHECK(g.r2 > 0.9 && g.r2 <= 1.0);

  LinearFit v;
  CHECK(!linear_fit({5, 5, 5}, {1, 2, 3}, v));
  CHECK(close_to(v.slope, 0.0));
  CHECK(close_to(v.intercept, 2.0));
  CHECK(close_to(v.r2, 0.0));

  LinearFit s;
  CHECK(!linear_fit({1}, {7}, s));
  CHECK(close_to(s.slope, 0.0) && close_to(s.intercept, 7.0));
  LinearFit e;
  CHECK(!linear_fit({}, {}, e));
  CHECK(close_to(e.slope, 0.0) && close_to(e.intercept, 0.0) &&
        close_to(e.r2, 0.0));
}

static void test_mad() {

  const std::vector<double> v = {10, 11, 12, 13, 14, 15, 100};
  CHECK(near_eps(mad(v), 1.4826 * 2.0, 1e-9));

  std::vector<size_t> out = mad_outliers(v, 3.0);
  CHECK(out.size() == 1);
  if (out.size() == 1) CHECK(out[0] == 6);

  CHECK(mad_outliers({1, 2, 3, 4, 5}, 3.0).empty());

  CHECK(close_to(mad({4, 4, 4, 4}), 0.0));
  CHECK(mad_outliers({4, 4, 4, 4}, 2.0).empty());

  CHECK(close_to(mad({}), 0.0));
  CHECK(mad_outliers({}, 2.0).empty());
}

static void test_autocorrelation() {

  std::vector<double> ramp;
  for (int i = 1; i <= 50; ++i) ramp.push_back(static_cast<double>(i));
  CHECK(near_eps(autocorrelation(ramp, 1), 0.94, 1e-9));
  CHECK(autocorrelation(ramp, 1) > 0.9);
  CHECK(autocorrelation(ramp, 2) > 0.0);

  const std::vector<double> alt = {1, -1, 1, -1, 1, -1, 1, -1, 1, -1};
  CHECK(autocorrelation(alt, 1) < 0.0);
  CHECK(near_eps(autocorrelation(alt, 1), -0.9, 1e-9));
  CHECK(autocorrelation(alt, 2) > 0.0);

  CHECK(close_to(autocorrelation(ramp, 0), 0.0));
  CHECK(close_to(autocorrelation(ramp, 50), 0.0));
  CHECK(close_to(autocorrelation(ramp, 999), 0.0));
  CHECK(close_to(autocorrelation({7, 7, 7, 7}, 1), 0.0));
  CHECK(close_to(autocorrelation({}, 1), 0.0));
  CHECK(close_to(autocorrelation({5}, 1), 0.0));
}

static void test_channel_adapter() {
  SchemaRegistry schemas;
  ChannelStore channels;
  build_channels(schemas, channels);

  std::vector<double> col = channel_column(channels, 1, 0);
  CHECK(col.size() == 3);
  if (col.size() == 3) {
    CHECK(close_to(col[0], 100.0) && close_to(col[1], 200.0) &&
          close_to(col[2], 300.0));
  }

  CHECK(close_to(median(col), 200.0));
  CHECK(close_to(variance(col, false), (100.0 * 100.0 * 2) / 3.0));

  CHECK(channel_column(channels, 99, 0).empty());
  CHECK(channel_column(channels, 1, 5).empty());
  CHECK(std::isnan(median(channel_column(channels, 99, 0))));
  CHECK(close_to(variance(channel_column(channels, 1, 5), false), 0.0));
}

int main() {
  test_percentile();
  test_moments();
  test_histogram();
  test_ewma();
  test_linear_fit();
  test_mad();
  test_autocorrelation();
  test_channel_adapter();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_stats_advanced: all checks passed\n");
  return 0;
}
