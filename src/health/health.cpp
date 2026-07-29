#include "blackspan/health.h"

#include <cmath>
#include <cstdio>

namespace blackspan {

namespace {

constexpr size_t kMaxSmoothWindow = 64;

size_t baseline_half_window(size_t n) { return n / 8; }

}

double volatility_score(const std::vector<double>& column) {
  const size_t n = column.size();
  if (n < 2) return 0.0;

  const size_t half = baseline_half_window(n);
  std::vector<double> win(kMaxSmoothWindow);

  double total_dev = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const size_t lo = i >= half ? i - half : 0;
    const size_t hi = (i + half < n) ? i + half : n - 1;
    const size_t w = hi - lo + 1;

    for (size_t j = 0; j < w; ++j) win[j] = column[lo + j];
    double sum = 0.0;
    for (size_t j = 0; j < w; ++j) sum += win[j];
    const double baseline = sum / static_cast<double>(w);

    total_dev += std::fabs(column[i] - baseline);
  }
  return total_dev / static_cast<double>(n);
}

std::vector<HealthFinding> channel_health_scan(const ChannelStore& store,
                                               double threshold) {
  std::vector<HealthFinding> out;
  for (uint16_t id : store.channels()) {
    const ChannelSeries* series = store.series(id);
    if (series == nullptr) continue;
    const ChannelSchema& schema = series->schema();
    for (size_t f = 0; f < schema.fields.size(); ++f) {
      const std::vector<double> col = store.column_as_doubles(id, f);
      HealthFinding finding;
      finding.channel_id = id;
      finding.field_index = f;
      finding.field_name = schema.fields[f].name;
      finding.samples = col.size();
      finding.score = volatility_score(col);
      finding.flagged = finding.score > threshold;
      out.push_back(std::move(finding));
    }
  }
  return out;
}

std::string describe_health(const HealthFinding& f) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6g", f.score);
  std::string out = "channel " + std::to_string(f.channel_id) + " field " +
                    f.field_name + " (" + std::to_string(f.samples) +
                    " samples) volatility " + buf;
  if (f.flagged) out += " [flagged]";
  return out;
}

}
