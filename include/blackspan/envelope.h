#ifndef BLACKSPAN_ENVELOPE_H
#define BLACKSPAN_ENVELOPE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

enum class EnvelopeMode : uint8_t {
  Centered = 0,
  Trailing = 1,
};

const char* envelope_mode_name(EnvelopeMode m);

bool parse_envelope_mode(const std::string& s, EnvelopeMode& out);

struct EnvelopePoint {
  uint64_t ts = 0;
  double lo = 0.0;
  double hi = 0.0;
  double mean = 0.0;
  double band = 0.0;
};

struct Envelope {
  std::vector<EnvelopePoint> points;
  double min_value = 0.0;
  double max_value = 0.0;
  double mean_value = 0.0;
};

Envelope compute_envelope(const std::vector<std::pair<uint64_t, double>>& series,
                          size_t window);

Envelope compute_envelope(const std::vector<std::pair<uint64_t, double>>& series,
                          size_t window, EnvelopeMode mode);

Envelope compute_sigma_envelope(
    const std::vector<std::pair<uint64_t, double>>& series, size_t window,
    double k, EnvelopeMode mode);

struct EnvelopeBreach {
  size_t index = 0;
  uint64_t ts = 0;
  double value = 0.0;
  double lo = 0.0;
  double hi = 0.0;
};

std::vector<EnvelopeBreach> envelope_breaches(
    const std::vector<std::pair<uint64_t, double>>& series, const Envelope& env);

struct BreachRun {
  size_t start_index = 0;
  size_t end_index = 0;
  size_t count = 0;
  uint64_t start_ts = 0;
  uint64_t end_ts = 0;
  double max_excess = 0.0;
};

std::vector<BreachRun> envelope_breach_runs(
    const std::vector<std::pair<uint64_t, double>>& series, const Envelope& env);

double envelope_coverage(const std::vector<std::pair<uint64_t, double>>& series,
                         const Envelope& env);

double envelope_mean_width(const Envelope& env);

double envelope_band_area(const Envelope& env);

EnvelopePoint envelope_at(const Envelope& env, uint64_t ts);

Envelope channel_envelope(const ChannelStore& store, uint16_t channel_id,
                          size_t field_index, size_t window);

Envelope channel_sigma_envelope(const ChannelStore& store, uint16_t channel_id,
                                size_t field_index, size_t window, double k);

std::string describe_envelope(const Envelope& env);

}

#endif
