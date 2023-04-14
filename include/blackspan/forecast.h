#ifndef BLACKSPAN_FORECAST_H
#define BLACKSPAN_FORECAST_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

enum class ForecastMethod : uint8_t {
  Mean = 0,
  Naive = 1,
  Drift = 2,
  LinearTrend = 3,
  Holt = 4,
  SeasonalNaive = 5,
};

const char* forecast_method_name(ForecastMethod m);

std::vector<double> forecast(const std::vector<double>& history,
                             ForecastMethod method, size_t horizon);

struct HoltState {
  double level = 0.0;
  double trend = 0.0;
};

HoltState holt_fit(const std::vector<double>& history, double alpha,
                   double beta);

std::vector<double> holt_forecast(const HoltState& s, size_t horizon);

struct BacktestResult {
  double mae = 0.0;
  double rmse = 0.0;
  double mape = 0.0;
  size_t tested = 0;
};

BacktestResult backtest(const std::vector<double>& history,
                        ForecastMethod method, size_t horizon);

std::vector<double> backtest_residuals(const std::vector<double>& history,
                                       ForecastMethod method, size_t horizon);

std::vector<BacktestResult> score_methods(const std::vector<double>& history,
                                          size_t horizon);

ForecastMethod best_method(const std::vector<double>& history, size_t horizon);

struct FieldForecast {
  uint16_t channel_id = 0;
  size_t field_index = 0;
  size_t samples = 0;
  ForecastMethod method = ForecastMethod::Mean;
  double next = 0.0;
  double rmse = 0.0;
};

std::vector<FieldForecast> forecast_channels(const ChannelStore& store,
                                             size_t horizon);

std::string describe_forecast(const FieldForecast& f);

std::string render_forecasts(const std::vector<FieldForecast>& forecasts);

}

#endif
