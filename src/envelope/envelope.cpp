#include <cmath>
#include <cstdio>
#include <deque>

#include "blackspan/aggregate.h"
#include "blackspan/envelope.h"

namespace blackspan {

namespace {

struct WindowStat {
  double lo = 0.0;
  double hi = 0.0;
  double mean = 0.0;
  double std = 0.0;
};

size_t clamp_window(size_t window, size_t n) {
  size_t w = window == 0 ? 1 : window;
  if (w > n) w = n;
  return w;
}

void window_bounds(size_t i, size_t n, size_t window, EnvelopeMode mode,
                   size_t& start, size_t& end) {
  if (mode == EnvelopeMode::Trailing) {
    end = i + 1;
    start = (i + 1 >= window) ? (i + 1 - window) : 0;
    return;
  }
  const size_t half = window / 2;
  start = (i >= half) ? (i - half) : 0;
  end = start + window;
  if (end > n) {
    end = n;
    start = (end >= window) ? (end - window) : 0;
  }
}

std::vector<WindowStat> sliding_stats(const std::vector<double>& v, size_t window,
                                      EnvelopeMode mode) {
  const size_t n = v.size();
  std::vector<WindowStat> out(n);
  if (n == 0) return out;

  std::deque<size_t> min_dq;
  std::deque<size_t> max_dq;
  double sum = 0.0;
  double sumsq = 0.0;
  size_t added = 0;
  size_t dropped = 0;

  for (size_t i = 0; i < n; ++i) {
    size_t start = 0;
    size_t end = 0;
    window_bounds(i, n, window, mode, start, end);

    while (added < end) {
      const double x = v[added];
      sum += x;
      sumsq += x * x;
      while (!min_dq.empty() && v[min_dq.back()] >= x) min_dq.pop_back();
      min_dq.push_back(added);
      while (!max_dq.empty() && v[max_dq.back()] <= x) max_dq.pop_back();
      max_dq.push_back(added);
      ++added;
    }
    while (dropped < start) {
      const double x = v[dropped];
      sum -= x;
      sumsq -= x * x;
      if (!min_dq.empty() && min_dq.front() == dropped) min_dq.pop_front();
      if (!max_dq.empty() && max_dq.front() == dropped) max_dq.pop_front();
      ++dropped;
    }

    const double count = static_cast<double>(end - start);
    const double mean = sum / count;
    double var = sumsq / count - mean * mean;
    if (var < 0.0) var = 0.0;

    WindowStat ws;
    ws.lo = v[min_dq.front()];
    ws.hi = v[max_dq.front()];
    ws.mean = mean;
    ws.std = std::sqrt(var);
    out[i] = ws;
  }
  return out;
}

void fill_overall(const std::vector<double>& v, Envelope& env) {
  if (v.empty()) return;
  double mn = v[0];
  double mx = v[0];
  double sum = 0.0;
  for (const double x : v) {
    if (x < mn) mn = x;
    if (x > mx) mx = x;
    sum += x;
  }
  env.min_value = mn;
  env.max_value = mx;
  env.mean_value = sum / static_cast<double>(v.size());
}

std::vector<double> values_of(
    const std::vector<std::pair<uint64_t, double>>& series) {
  std::vector<double> v;
  v.reserve(series.size());
  for (const auto& s : series) v.push_back(s.second);
  return v;
}

double lerp(double a, double b, double f) { return a + (b - a) * f; }

}

const char* envelope_mode_name(EnvelopeMode m) {
  switch (m) {
    case EnvelopeMode::Centered: return "centered";
    case EnvelopeMode::Trailing: return "trailing";
  }
  return "unknown";
}

bool parse_envelope_mode(const std::string& s, EnvelopeMode& out) {
  if (s == "centered") {
    out = EnvelopeMode::Centered;
    return true;
  }
  if (s == "trailing") {
    out = EnvelopeMode::Trailing;
    return true;
  }
  return false;
}

Envelope compute_envelope(const std::vector<std::pair<uint64_t, double>>& series,
                          size_t window, EnvelopeMode mode) {
  Envelope env;
  const size_t n = series.size();
  if (n == 0) return env;

  const std::vector<double> v = values_of(series);
  fill_overall(v, env);

  const size_t w = clamp_window(window, n);
  const std::vector<WindowStat> stats = sliding_stats(v, w, mode);

  env.points.resize(n);
  for (size_t i = 0; i < n; ++i) {
    EnvelopePoint p;
    p.ts = series[i].first;
    p.lo = stats[i].lo;
    p.hi = stats[i].hi;
    p.mean = stats[i].mean;
    p.band = stats[i].std;
    env.points[i] = p;
  }
  return env;
}

Envelope compute_envelope(const std::vector<std::pair<uint64_t, double>>& series,
                          size_t window) {
  return compute_envelope(series, window, EnvelopeMode::Centered);
}

Envelope compute_sigma_envelope(
    const std::vector<std::pair<uint64_t, double>>& series, size_t window,
    double k, EnvelopeMode mode) {
  Envelope env;
  const size_t n = series.size();
  if (n == 0) return env;

  const double kk = k < 0.0 ? -k : k;
  const std::vector<double> v = values_of(series);
  fill_overall(v, env);

  const size_t w = clamp_window(window, n);
  const std::vector<WindowStat> stats = sliding_stats(v, w, mode);

  env.points.resize(n);
  for (size_t i = 0; i < n; ++i) {
    EnvelopePoint p;
    p.ts = series[i].first;
    p.mean = stats[i].mean;
    p.band = stats[i].std;
    p.lo = stats[i].mean - kk * stats[i].std;
    p.hi = stats[i].mean + kk * stats[i].std;
    env.points[i] = p;
  }
  return env;
}

std::vector<EnvelopeBreach> envelope_breaches(
    const std::vector<std::pair<uint64_t, double>>& series,
    const Envelope& env) {
  std::vector<EnvelopeBreach> out;
  const size_t m =
      series.size() < env.points.size() ? series.size() : env.points.size();
  for (size_t i = 0; i < m; ++i) {
    const double value = series[i].second;
    const EnvelopePoint& p = env.points[i];
    if (value < p.lo || value > p.hi) {
      EnvelopeBreach b;
      b.index = i;
      b.ts = series[i].first;
      b.value = value;
      b.lo = p.lo;
      b.hi = p.hi;
      out.push_back(b);
    }
  }
  return out;
}

std::vector<BreachRun> envelope_breach_runs(
    const std::vector<std::pair<uint64_t, double>>& series,
    const Envelope& env) {
  std::vector<BreachRun> runs;
  const std::vector<EnvelopeBreach> breaches = envelope_breaches(series, env);
  if (breaches.empty()) return runs;

  auto excess_of = [](const EnvelopeBreach& b) {
    const double below = b.lo - b.value;
    const double above = b.value - b.hi;
    return below > above ? below : above;
  };

  BreachRun cur;
  bool open = false;
  for (const EnvelopeBreach& b : breaches) {
    const double ex = excess_of(b);
    if (open && b.index == cur.end_index + 1) {
      cur.end_index = b.index;
      cur.end_ts = b.ts;
      ++cur.count;
      if (ex > cur.max_excess) cur.max_excess = ex;
      continue;
    }
    if (open) runs.push_back(cur);
    cur = BreachRun{};
    cur.start_index = b.index;
    cur.end_index = b.index;
    cur.count = 1;
    cur.start_ts = b.ts;
    cur.end_ts = b.ts;
    cur.max_excess = ex;
    open = true;
  }
  if (open) runs.push_back(cur);
  return runs;
}

double envelope_coverage(const std::vector<std::pair<uint64_t, double>>& series,
                         const Envelope& env) {
  const size_t m =
      series.size() < env.points.size() ? series.size() : env.points.size();
  if (m == 0) return 1.0;
  size_t inside = 0;
  for (size_t i = 0; i < m; ++i) {
    const double value = series[i].second;
    const EnvelopePoint& p = env.points[i];
    if (value >= p.lo && value <= p.hi) ++inside;
  }
  return static_cast<double>(inside) / static_cast<double>(m);
}

double envelope_mean_width(const Envelope& env) {
  if (env.points.empty()) return 0.0;
  double sum = 0.0;
  for (const EnvelopePoint& p : env.points) sum += p.hi - p.lo;
  return sum / static_cast<double>(env.points.size());
}

double envelope_band_area(const Envelope& env) {
  if (env.points.size() < 2) return 0.0;
  double area = 0.0;
  for (size_t i = 0; i + 1 < env.points.size(); ++i) {
    const EnvelopePoint& a = env.points[i];
    const EnvelopePoint& b = env.points[i + 1];
    const double wa = a.hi - a.lo;
    const double wb = b.hi - b.lo;
    const double dt = static_cast<double>(b.ts) - static_cast<double>(a.ts);
    area += 0.5 * (wa + wb) * dt;
  }
  return area;
}

EnvelopePoint envelope_at(const Envelope& env, uint64_t ts) {
  EnvelopePoint result;
  if (env.points.empty()) return result;
  if (ts <= env.points.front().ts) return env.points.front();
  if (ts >= env.points.back().ts) return env.points.back();

  size_t lo = 0;
  size_t hi = env.points.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (env.points[mid].ts < ts) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return env.points.front();
  if (lo >= env.points.size()) return env.points.back();
  if (env.points[lo].ts == ts) return env.points[lo];

  const EnvelopePoint& a = env.points[lo - 1];
  const EnvelopePoint& b = env.points[lo];
  const double denom = static_cast<double>(b.ts) - static_cast<double>(a.ts);
  if (denom == 0.0) return a;
  const double f = (static_cast<double>(ts) - static_cast<double>(a.ts)) / denom;

  result.ts = ts;
  result.lo = lerp(a.lo, b.lo, f);
  result.hi = lerp(a.hi, b.hi, f);
  result.mean = lerp(a.mean, b.mean, f);
  result.band = lerp(a.band, b.band, f);
  return result;
}

Envelope channel_envelope(const ChannelStore& store, uint16_t channel_id,
                          size_t field_index, size_t window) {
  return compute_envelope(
      column_with_timestamps(store, channel_id, field_index), window);
}

Envelope channel_sigma_envelope(const ChannelStore& store, uint16_t channel_id,
                                size_t field_index, size_t window, double k) {
  return compute_sigma_envelope(
      column_with_timestamps(store, channel_id, field_index), window, k,
      EnvelopeMode::Centered);
}

std::string describe_envelope(const Envelope& env) {
  if (env.points.empty()) return "envelope: empty\n";
  double band_sum = 0.0;
  for (const EnvelopePoint& p : env.points) band_sum += p.band;
  const double mean_band = band_sum / static_cast<double>(env.points.size());
  char buf[192];
  std::snprintf(buf, sizeof(buf),
                "envelope: %zu points min=%.6g max=%.6g mean=%.6g band=%.6g\n",
                env.points.size(), env.min_value, env.max_value, env.mean_value,
                mean_band);
  return buf;
}

}
