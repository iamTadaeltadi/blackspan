#include "blackspan/driftmodel.h"

#include <cmath>

namespace blackspan {

std::vector<double> derived_residuals(const DecodedRecording& rec,
                                      const CachedDerived& d) {
  std::vector<double> out;
  const std::vector<double> col =
      rec.channels().column_as_doubles(d.primary_channel, 0);
  out.reserve(col.size());
  for (size_t i = 0; i < col.size(); ++i) {
    out.push_back(col[i] - d.values[i]);
  }
  return out;
}

std::vector<ChannelDrift> channel_drift(const DecodedRecording& rec) {
  std::vector<ChannelDrift> out;
  for (const CachedDerived& d : rec.derived()) {
    const std::vector<double> res = derived_residuals(rec, d);
    ChannelDrift drift;
    drift.derived_id = d.derived_id;
    drift.primary_channel = d.primary_channel;
    drift.samples = res.size();
    double sq = 0.0;
    double mx = 0.0;
    for (double r : res) {
      sq += r * r;
      const double a = std::fabs(r);
      if (a > mx) mx = a;
    }
    if (!res.empty()) {
      drift.residual_rms = std::sqrt(sq / static_cast<double>(res.size()));
    }
    drift.max_abs = mx;
    out.push_back(drift);
  }
  return out;
}

std::string describe_drift(const ChannelDrift& d) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6g", d.residual_rms);
  return "derived " + std::to_string(d.derived_id) + " over channel " +
         std::to_string(d.primary_channel) + " (" + std::to_string(d.samples) +
         " samples) rms " + buf;
}

}
