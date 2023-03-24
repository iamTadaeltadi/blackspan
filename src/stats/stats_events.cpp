#include <limits>

#include "blackspan/stats.h"

namespace blackspan {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}

double fault_rate(const AnomalyEngine& engine, uint64_t window_ns) {
  const std::vector<FaultEvent>& faults = engine.faults();
  if (faults.empty()) return 0.0;

  uint64_t lo = faults[0].monotonic_ts;
  uint64_t hi = faults[0].monotonic_ts;
  for (const FaultEvent& f : faults) {
    if (f.monotonic_ts < lo) lo = f.monotonic_ts;
    if (f.monotonic_ts > hi) hi = f.monotonic_ts;
  }

  const double span_ns = static_cast<double>(hi - lo);
  if (span_ns == 0.0) return kNaN;

  return (static_cast<double>(faults.size()) / span_ns) *
         static_cast<double>(window_ns);
}

double mean_interfault_ns(const AnomalyEngine& engine) {
  const std::vector<FaultEvent>& faults = engine.faults();
  if (faults.size() < 2) return kNaN;

  double total_gap = 0.0;
  for (size_t i = 1; i < faults.size(); ++i) {
    total_gap += static_cast<double>(faults[i].monotonic_ts) -
                 static_cast<double>(faults[i - 1].monotonic_ts);
  }
  return total_gap / static_cast<double>(faults.size() - 1);
}

std::array<size_t, 4> severity_histogram(const AnomalyEngine& engine) {
  std::array<size_t, 4> counts{};
  for (const FaultEvent& f : engine.faults()) {
    ++counts[static_cast<size_t>(f.severity)];
  }
  return counts;
}

}
