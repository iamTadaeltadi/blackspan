#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/byteio.h"
#include "blackspan/channel.h"
#include "blackspan/forecast.h"
#include "blackspan/frame.h"
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

void build_ramp_channel(SchemaRegistry& schemas, ChannelStore& channels) {
  std::vector<uint8_t> wire;
  {
    ByteWriter p;
    p.u16(1);
    p.u8(1);
    p.u8(1);
    const std::string cname = "altitude";
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
    p.u16(12);
    p.u16(0);
    for (uint16_t v = 100; v <= 210; v = static_cast<uint16_t>(v + 10)) {
      p.u16(v);
    }
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

static void test_method_names() {
  CHECK(std::string(forecast_method_name(ForecastMethod::Mean)) == "mean");
  CHECK(std::string(forecast_method_name(ForecastMethod::Naive)) == "naive");
  CHECK(std::string(forecast_method_name(ForecastMethod::Drift)) == "drift");
  CHECK(std::string(forecast_method_name(ForecastMethod::LinearTrend)) ==
        "linear-trend");
  CHECK(std::string(forecast_method_name(ForecastMethod::Holt)) == "holt");
  CHECK(std::string(forecast_method_name(ForecastMethod::SeasonalNaive)) ==
        "seasonal-naive");
}

static void test_basic_methods() {
  std::vector<double> ramp;
  for (int i = 0; i < 10; ++i) ramp.push_back(static_cast<double>(i));

  std::vector<double> naive = forecast(ramp, ForecastMethod::Naive, 3);
  CHECK(naive.size() == 3);
  for (double v : naive) CHECK(close_to(v, 9.0));

  std::vector<double> drift = forecast(ramp, ForecastMethod::Drift, 3);
  CHECK(drift.size() == 3);
  CHECK(close_to(drift[0], 10.0) && close_to(drift[1], 11.0) &&
        close_to(drift[2], 12.0));

  std::vector<double> lin = forecast(ramp, ForecastMethod::LinearTrend, 4);
  CHECK(lin.size() == 4);
  CHECK(close_to(lin[0], 10.0) && close_to(lin[1], 11.0) &&
        close_to(lin[2], 12.0) && close_to(lin[3], 13.0));

  std::vector<double> mean = forecast(ramp, ForecastMethod::Mean, 2);
  CHECK(mean.size() == 2);
  CHECK(close_to(mean[0], 4.5) && close_to(mean[1], 4.5));

  std::vector<double> offset_ramp;
  for (int i = 0; i < 8; ++i)
    offset_ramp.push_back(3.0 + 2.5 * static_cast<double>(i));
  std::vector<double> od = forecast(offset_ramp, ForecastMethod::Drift, 2);
  std::vector<double> ol = forecast(offset_ramp, ForecastMethod::LinearTrend, 2);
  CHECK(close_to(od[0], 3.0 + 2.5 * 8.0));
  CHECK(close_to(od[1], 3.0 + 2.5 * 9.0));
  CHECK(close_to(ol[0], 3.0 + 2.5 * 8.0));
  CHECK(close_to(ol[1], 3.0 + 2.5 * 9.0));
}

static void test_holt() {
  std::vector<double> line;
  for (int i = 0; i < 20; ++i)
    line.push_back(3.0 + 2.0 * static_cast<double>(i));

  HoltState s = holt_fit(line, 0.5, 0.1);
  CHECK(near_eps(s.trend, 2.0, 1e-6));
  CHECK(near_eps(s.level, line.back(), 1e-6));

  std::vector<double> hf = holt_forecast(s, 5);
  CHECK(hf.size() == 5);
  for (size_t i = 0; i < hf.size(); ++i) {
    const double expected = 3.0 + 2.0 * static_cast<double>(20 + i);
    CHECK(near_eps(hf[i], expected, 1e-6));
  }

  std::vector<double> via = forecast(line, ForecastMethod::Holt, 3);
  CHECK(via.size() == 3);
  CHECK(near_eps(via[0], 43.0, 1e-6));
  CHECK(near_eps(via[1], 45.0, 1e-6));
  CHECK(near_eps(via[2], 47.0, 1e-6));

  HoltState empty = holt_fit({}, 0.5, 0.1);
  CHECK(close_to(empty.level, 0.0) && close_to(empty.trend, 0.0));
  HoltState one = holt_fit({5.0}, 0.5, 0.1);
  CHECK(close_to(one.level, 5.0) && close_to(one.trend, 0.0));
  CHECK(holt_forecast(s, 0).empty());
}

static void test_seasonal() {
  std::vector<double> season;
  for (int c = 0; c < 6; ++c) {
    season.push_back(1.0);
    season.push_back(5.0);
    season.push_back(3.0);
  }
  std::vector<double> sn = forecast(season, ForecastMethod::SeasonalNaive, 6);
  CHECK(sn.size() == 6);
  const double cycle[3] = {1.0, 5.0, 3.0};
  for (size_t i = 0; i < sn.size(); ++i) {
    CHECK(close_to(sn[i], cycle[i % 3]));
  }

  std::vector<double> too_short = {7.0, 8.0};
  std::vector<double> fb =
      forecast(too_short, ForecastMethod::SeasonalNaive, 2);
  CHECK(fb.size() == 2);
  CHECK(close_to(fb[0], 8.0) && close_to(fb[1], 8.0));
}

static void test_backtest() {
  std::vector<double> ramp;
  for (int i = 0; i < 12; ++i) ramp.push_back(static_cast<double>(i));

  BacktestResult drift = backtest(ramp, ForecastMethod::Drift, 3);
  CHECK(drift.tested == 3);
  CHECK(near_eps(drift.mae, 0.0, 1e-9));
  CHECK(near_eps(drift.rmse, 0.0, 1e-9));

  BacktestResult lin = backtest(ramp, ForecastMethod::LinearTrend, 3);
  CHECK(lin.tested == 3);
  CHECK(near_eps(lin.rmse, 0.0, 1e-9));

  BacktestResult mean = backtest(ramp, ForecastMethod::Mean, 3);
  CHECK(mean.tested == 3);
  CHECK(mean.rmse > 0.0);
  CHECK(mean.mae > 0.0);

  std::vector<double> zeros_line = {5.0, 4.0, 3.0, 2.0, 1.0, 0.0};
  BacktestResult guarded = backtest(zeros_line, ForecastMethod::Mean, 3);
  CHECK(guarded.tested == 3);
  CHECK(std::isfinite(guarded.mape));
  CHECK(near_eps(guarded.mape, 200.0, 1e-9));

  BacktestResult short_hist = backtest({1.0, 2.0}, ForecastMethod::Drift, 5);
  CHECK(short_hist.tested == 0);
  CHECK(close_to(short_hist.mae, 0.0) && close_to(short_hist.rmse, 0.0) &&
        close_to(short_hist.mape, 0.0));

  BacktestResult zero_horizon = backtest(ramp, ForecastMethod::Drift, 0);
  CHECK(zero_horizon.tested == 0);
}

static void test_best_method() {
  std::vector<double> ramp;
  for (int i = 0; i < 12; ++i) ramp.push_back(static_cast<double>(i));
  ForecastMethod ramp_best = best_method(ramp, 3);
  CHECK(ramp_best == ForecastMethod::Drift ||
        ramp_best == ForecastMethod::LinearTrend);

  std::vector<double> flat(10, 5.0);
  ForecastMethod flat_best = best_method(flat, 2);
  CHECK(flat_best == ForecastMethod::Mean ||
        flat_best == ForecastMethod::Naive);

  ForecastMethod degenerate = best_method({}, 3);
  CHECK(degenerate == ForecastMethod::Mean);
}

static void test_degenerate() {
  std::vector<double> empty_drift = forecast({}, ForecastMethod::Drift, 3);
  CHECK(empty_drift.size() == 3);
  for (double v : empty_drift) CHECK(close_to(v, 0.0));

  std::vector<double> empty_lin = forecast({}, ForecastMethod::LinearTrend, 3);
  CHECK(empty_lin.size() == 3);
  for (double v : empty_lin) CHECK(close_to(v, 0.0));

  CHECK(forecast({1.0, 2.0, 3.0}, ForecastMethod::Drift, 0).empty());

  const ForecastMethod methods[] = {
      ForecastMethod::Mean,  ForecastMethod::Naive,
      ForecastMethod::Drift, ForecastMethod::LinearTrend,
      ForecastMethod::Holt,  ForecastMethod::SeasonalNaive};
  for (ForecastMethod m : methods) {
    std::vector<double> single = forecast({7.0}, m, 2);
    CHECK(single.size() == 2);
    CHECK(close_to(single[0], 7.0) && close_to(single[1], 7.0));
  }

  std::vector<double> constant(6, 4.0);
  std::vector<double> lin_const = forecast(constant, ForecastMethod::LinearTrend, 3);
  for (double v : lin_const) CHECK(close_to(v, 4.0));
  std::vector<double> holt_const = forecast(constant, ForecastMethod::Holt, 3);
  for (double v : holt_const) CHECK(near_eps(v, 4.0, 1e-9));
}

static void test_scoring() {
  std::vector<double> ramp;
  for (int i = 0; i < 12; ++i) ramp.push_back(static_cast<double>(i));

  std::vector<BacktestResult> scores = score_methods(ramp, 3);
  CHECK(scores.size() == 6);
  CHECK(near_eps(scores[2].rmse, 0.0, 1e-9));
  CHECK(near_eps(scores[3].rmse, 0.0, 1e-9));
  CHECK(scores[0].rmse > 0.0);

  std::vector<double> drift_resid =
      backtest_residuals(ramp, ForecastMethod::Drift, 3);
  CHECK(drift_resid.size() == 3);
  for (double r : drift_resid) CHECK(near_eps(r, 0.0, 1e-9));

  std::vector<double> mean_resid =
      backtest_residuals(ramp, ForecastMethod::Mean, 3);
  CHECK(mean_resid.size() == 3);
  double abs_total = 0.0;
  for (double r : mean_resid) abs_total += std::fabs(r);
  CHECK(abs_total > 0.0);

  CHECK(backtest_residuals({1.0, 2.0}, ForecastMethod::Drift, 5).empty());
  CHECK(backtest_residuals(ramp, ForecastMethod::Drift, 0).empty());
}

static void test_adapter() {
  SchemaRegistry schemas;
  ChannelStore channels;
  build_ramp_channel(schemas, channels);

  std::vector<FieldForecast> results = forecast_channels(channels, 3);
  CHECK(results.size() == 1);
  if (results.size() == 1) {
    const FieldForecast& f = results[0];
    CHECK(f.channel_id == 1);
    CHECK(f.field_index == 0);
    CHECK(f.samples == 12);
    CHECK(f.method == ForecastMethod::Drift ||
          f.method == ForecastMethod::LinearTrend);
    CHECK(near_eps(f.next, 220.0, 1e-6));
    CHECK(near_eps(f.rmse, 0.0, 1e-6));

    std::string text = describe_forecast(f);
    CHECK(text.find("channel 0001") != std::string::npos);
    CHECK(text.find("field 0") != std::string::npos);
    CHECK(text.find("samples 12") != std::string::npos);
  }

  std::string rendered = render_forecasts(results);
  CHECK(rendered.find("1 forecast(s)") != std::string::npos);
  CHECK(rendered.find("channel 0001") != std::string::npos);
  CHECK(render_forecasts({}).find("0 forecast(s)") != std::string::npos);

  ChannelStore empty_store;
  CHECK(forecast_channels(empty_store, 3).empty());
}

int main() {
  test_method_names();
  test_basic_methods();
  test_holt();
  test_seasonal();
  test_backtest();
  test_best_method();
  test_degenerate();
  test_scoring();
  test_adapter();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_forecast: all checks passed\n");
  return 0;
}
