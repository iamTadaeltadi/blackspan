#ifndef BLACKSPAN_DETECT_H
#define BLACKSPAN_DETECT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

enum class DetectKind : uint8_t {
  ThresholdHigh,
  ThresholdLow,
  OutOfRange,
  RateOfChange,
  StuckAt,
  Spike,
};

const char* detect_kind_name(DetectKind k);

struct DetectRule {
  uint32_t id = 0;
  uint16_t channel_id = 0;
  size_t field_index = 0;
  DetectKind kind = DetectKind::ThresholdHigh;
  double a = 0.0;
  double b = 0.0;
  size_t window = 0;
};

struct Detection {
  uint32_t rule_id = 0;
  uint16_t channel_id = 0;
  size_t field_index = 0;
  DetectKind kind = DetectKind::ThresholdHigh;
  size_t sample_index = 0;
  uint64_t ts = 0;
  double value = 0.0;
  double magnitude = 0.0;
};

void evaluate_rule(const std::vector<std::pair<uint64_t, double>>& series,
                   const DetectRule& rule, std::vector<Detection>& out);

enum class DetectParseError : uint8_t {
  None,
  Empty,
  BadKind,
  MissingField,
  BadNumber,

  BadField,
};

const char* detect_parse_error_str(DetectParseError e);

struct DetectRuleParse {
  std::vector<DetectRule> rules;
  std::vector<std::pair<size_t, DetectParseError>> errors;

  bool ok() const { return errors.empty(); }
};

DetectRuleParse parse_detect_rules(const std::string& text);

std::vector<Detection> run_detectors(const ChannelStore& store,
                                     const std::vector<DetectRule>& rules);

}

#endif
