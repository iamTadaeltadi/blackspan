#include "blackspan/forecast.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "blackspan/fold.h"
#include "blackspan/stats_advanced.h"

namespace blackspan {

namespace {

constexpr double kHoltAlpha = 0.5;
constexpr double kHoltBeta = 0.1;
constexpr double kTieEpsilon = 1e-9;
constexpr size_t kHoltWarmup = 6;
constexpr size_t kSeasonalMinPeriod = 2;

constexpr ForecastMethod kAllMethods[] = {
    ForecastMethod::Mean,  ForecastMethod::Naive,
    ForecastMethod::Drift, ForecastMethod::LinearTrend,
    ForecastMethod::Holt,  ForecastMethod::SeasonalNaive};

double mean_value(const std::vector<double>& xs) {
  if (xs.empty()) return 0.0;
  double sum = 0.0;
  for (const double v : xs) sum += v;
  return sum / static_cast<double>(xs.size());
}

double average_step(const std::vector<double>& xs) {
  const size_t n = xs.size();
  if (n < 2) return 0.0;
  return (xs.back() - xs.front()) / static_cast<double>(n - 1);
}

std::vector<double> index_axis(size_t n) {
  std::vector<double> axis(n);
  for (size_t i = 0; i < n; ++i) axis[i] = static_cast<double>(i);
  return axis;
}

std::vector<double> flat_forecast(double value, size_t horizon) {
  return std::vector<double>(horizon, value);
}

std::vector<double> forecast_mean(const std::vector<double>& history,
                                  size_t horizon) {
  return flat_forecast(mean_value(history), horizon);
}

std::vector<double> forecast_naive(const std::vector<double>& history,
                                   size_t horizon) {
  return flat_forecast(history.back(), horizon);
}

std::vector<double> forecast_drift(const std::vector<double>& history,
                                   size_t horizon) {
  const double last = history.back();
  const double step = average_step(history);
  std::vector<double> out(horizon);
  for (size_t i = 0; i < horizon; ++i) {
    out[i] = last + step * static_cast<double>(i + 1);
  }
  return out;
}

std::vector<double> forecast_linear(const std::vector<double>& history,
                                    size_t horizon) {
  const size_t n = history.size();
  const std::vector<double> axis = index_axis(n);
  LinearFit fit;
  linear_fit(axis, history, fit);
  std::vector<double> out(horizon);
  for (size_t i = 0; i < horizon; ++i) {
    const double x = static_cast<double>(n + i);
    out[i] = fit.slope * x + fit.intercept;
  }
  return out;
}

std::vector<double> forecast_seasonal(const std::vector<double>& history,
                                      size_t horizon) {
  const size_t n = history.size();
  const PeriodEstimate est = estimate_period(history, kSeasonalMinPeriod, n);
  if (!est.found || est.period == 0 || est.period > n) {
    return forecast_naive(history, horizon);
  }
  const size_t period = est.period;
  const size_t base = n - period;
  std::vector<double> out(horizon);
  for (size_t i = 0; i < horizon; ++i) {
    out[i] = history[base + (i % period)];
  }
  return out;
}

struct ErrorTotals {
  double abs_sum = 0.0;
  double sq_sum = 0.0;
  double pct_sum = 0.0;
  size_t count = 0;
  size_t pct_count = 0;
};

ErrorTotals accumulate_errors(const std::vector<double>& predicted,
                              const std::vector<double>& actual) {
  ErrorTotals totals;
  const size_t count = std::min(predicted.size(), actual.size());
  for (size_t i = 0; i < count; ++i) {
    const double error = predicted[i] - actual[i];
    totals.abs_sum += std::fabs(error);
    totals.sq_sum += error * error;
    if (actual[i] != 0.0) {
      totals.pct_sum += std::fabs(error / actual[i]);
      ++totals.pct_count;
    }
  }
  totals.count = count;
  return totals;
}

std::vector<double> slice(const std::vector<double>& xs, size_t begin,
                          size_t end) {
  const auto first = xs.begin() + static_cast<std::ptrdiff_t>(begin);
  const auto last = xs.begin() + static_cast<std::ptrdiff_t>(end);
  return std::vector<double>(first, last);
}

}

const char* forecast_method_name(ForecastMethod m) {
  switch (m) {
    case ForecastMethod::Mean:
      return "mean";
    case ForecastMethod::Naive:
      return "naive";
    case ForecastMethod::Drift:
      return "drift";
    case ForecastMethod::LinearTrend:
      return "linear-trend";
    case ForecastMethod::Holt:
      return "holt";
    case ForecastMethod::SeasonalNaive:
      return "seasonal-naive";
  }
  return "unknown";
}

HoltState holt_fit(const std::vector<double>& history, double alpha,
                   double beta) {
  HoltState state;
  const size_t n = history.size();
  if (n == 0) return state;
  if (n == 1) {
    state.level = history[0];
    return state;
  }

  const size_t warmup = std::min(n, kHoltWarmup);
  const std::vector<double> axis = index_axis(warmup);
  const std::vector<double> head = slice(history, 0, warmup);
  LinearFit fit;
  if (linear_fit(axis, head, fit)) {
    state.level = fit.intercept;
    state.trend = fit.slope;
  } else {
    state.level = history[0];
    state.trend = history[1] - history[0];
  }

  for (size_t t = 1; t < n; ++t) {
    const double prev_level = state.level;
    const double observed = history[t];
    const double next_level =
        alpha * observed + (1.0 - alpha) * (prev_level + state.trend);
    const double next_trend =
        beta * (next_level - prev_level) + (1.0 - beta) * state.trend;
    state.level = next_level;
    state.trend = next_trend;
  }
  return state;
}

std::vector<double> holt_forecast(const HoltState& s, size_t horizon) {
  std::vector<double> out(horizon);
  for (size_t i = 0; i < horizon; ++i) {
    out[i] = s.level + static_cast<double>(i + 1) * s.trend;
  }
  return out;
}

std::vector<double> forecast(const std::vector<double>& history,
                             ForecastMethod method, size_t horizon) {
  if (horizon == 0) return {};
  if (history.empty()) return std::vector<double>(horizon, 0.0);

  switch (method) {
    case ForecastMethod::Mean:
      return forecast_mean(history, horizon);
    case ForecastMethod::Naive:
      return forecast_naive(history, horizon);
    case ForecastMethod::Drift:
      return forecast_drift(history, horizon);
    case ForecastMethod::LinearTrend:
      return forecast_linear(history, horizon);
    case ForecastMethod::Holt:
      return holt_forecast(holt_fit(history, kHoltAlpha, kHoltBeta), horizon);
    case ForecastMethod::SeasonalNaive:
      return forecast_seasonal(history, horizon);
  }
  return std::vector<double>(horizon, 0.0);
}

BacktestResult backtest(const std::vector<double>& history,
                        ForecastMethod method, size_t horizon) {
  BacktestResult result;
  if (horizon == 0 || history.size() <= horizon) return result;

  const size_t split = history.size() - horizon;
  const std::vector<double> prefix = slice(history, 0, split);
  const std::vector<double> actual = slice(history, split, history.size());
  const std::vector<double> predicted = forecast(prefix, method, horizon);

  const ErrorTotals totals = accumulate_errors(predicted, actual);
  if (totals.count == 0) return result;

  const double denom = static_cast<double>(totals.count);
  result.mae = totals.abs_sum / denom;
  result.rmse = std::sqrt(totals.sq_sum / denom);
  result.mape = totals.pct_count == 0
                    ? 0.0
                    : (totals.pct_sum / static_cast<double>(totals.pct_count)) *
                          100.0;
  result.tested = totals.count;
  return result;
}

std::vector<double> backtest_residuals(const std::vector<double>& history,
                                       ForecastMethod method, size_t horizon) {
  std::vector<double> residuals;
  if (horizon == 0 || history.size() <= horizon) return residuals;

  const size_t split = history.size() - horizon;
  const std::vector<double> prefix = slice(history, 0, split);
  const std::vector<double> actual = slice(history, split, history.size());
  const std::vector<double> predicted = forecast(prefix, method, horizon);

  const size_t count = std::min(predicted.size(), actual.size());
  residuals.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    residuals.push_back(predicted[i] - actual[i]);
  }
  return residuals;
}

std::vector<BacktestResult> score_methods(const std::vector<double>& history,
                                          size_t horizon) {
  std::vector<BacktestResult> scores;
  scores.reserve(sizeof(kAllMethods) / sizeof(kAllMethods[0]));
  for (const ForecastMethod method : kAllMethods) {
    scores.push_back(backtest(history, method, horizon));
  }
  return scores;
}

ForecastMethod best_method(const std::vector<double>& history, size_t horizon) {
  const std::vector<BacktestResult> scores = score_methods(history, horizon);
  ForecastMethod best = kAllMethods[0];
  double best_rmse = std::numeric_limits<double>::infinity();
  bool have = false;
  for (size_t i = 0; i < scores.size(); ++i) {
    if (!have || scores[i].rmse < best_rmse - kTieEpsilon) {
      best = kAllMethods[i];
      best_rmse = scores[i].rmse;
      have = true;
    }
  }
  return best;
}

std::vector<FieldForecast> forecast_channels(const ChannelStore& store,
                                             size_t horizon) {
  std::vector<FieldForecast> out;
  const std::vector<uint16_t> ids = store.channels();
  for (const uint16_t id : ids) {
    const ChannelSeries* series = store.series(id);
    if (series == nullptr) continue;
    const size_t field_count = series->schema().fields.size();
    for (size_t field = 0; field < field_count; ++field) {
      const std::vector<double> values = channel_column(store, id, field);

      FieldForecast summary;
      summary.channel_id = id;
      summary.field_index = field;
      summary.samples = values.size();
      summary.method = best_method(values, horizon);

      const std::vector<double> projection =
          forecast(values, summary.method, horizon);
      summary.next = projection.empty() ? 0.0 : projection.front();
      summary.rmse = backtest(values, summary.method, horizon).rmse;
      out.push_back(summary);
    }
  }
  return out;
}

std::string describe_forecast(const FieldForecast& f) {
  char buf[192];
  std::snprintf(
      buf, sizeof(buf),
      "channel %04x field %zu samples %zu method %s next %.6g rmse %.6g",
      static_cast<unsigned>(f.channel_id), f.field_index, f.samples,
      forecast_method_name(f.method), f.next, f.rmse);
  return std::string(buf);
}

std::string render_forecasts(const std::vector<FieldForecast>& forecasts) {
  char head[48];
  std::snprintf(head, sizeof(head), "%zu forecast(s)\n", forecasts.size());
  std::string out = head;
  for (const FieldForecast& f : forecasts) {
    out += "  ";
    out += describe_forecast(f);
    out += "\n";
  }
  return out;
}

}
