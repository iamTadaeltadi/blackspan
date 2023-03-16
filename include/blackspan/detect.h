#ifndef BLACKSPAN_DETECT_H
#define BLACKSPAN_DETECT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/channel.h"

// The detect subsystem turns a channel field's raw (timestamp, value) series
// into typed detection events: the layer that says "here the bus voltage went
// out of range", "the sensor stuck", or "a spike fired". A recorder stores what
// its firmware sampled; triage wants the handful of moments that matter, and a
// site expresses which moments those are as a small set of detector rules.
//
// The core evaluators run on a plain std::vector<std::pair<uint64_t,double>>
// series (each pair is a sample's block timestamp and its value in whatever
// units the caller supplies), so they are exhaustively unit-testable without a
// recording. A thin ChannelStore adapter pulls a channel field's series exactly
// as the aggregate subsystem does and drives the same evaluators.
//
// Rules are authored in a line-oriented text format so a site's alarm limits
// can live beside its recordings and be version-controlled. Parsing reports
// every malformed line with its 1-based line number rather than stopping at the
// first. Everything here reads its inputs only; it mutates nothing, touches no
// raw bytes, and never throws. Degenerate inputs (an unknown channel, a zero
// window, a stddev of zero) simply yield no detections.

namespace blackspan {

// The condition a detector rule tests, one firing per qualifying sample.
enum class DetectKind : uint8_t {
  ThresholdHigh,  // value > limit           (strictly)
  ThresholdLow,   // value < limit
  OutOfRange,     // value < lo || value > hi
  RateOfChange,   // |value[i]-value[i-1]| / max(dt,1) > limit  (per-ns rate)
  StuckAt,        // |value[i]-value[i-1]| <= epsilon for >= run_len consec steps
  Spike,          // |value - windowed_mean| > k * windowed_stddev  (sliding win)
};

// Lowercase mnemonic for a kind ("high", "low", "range", "rate", "stuck",
// "spike"); matches the rule-text keyword. Never null.
const char* detect_kind_name(DetectKind k);

// One detector rule. Each kind reads only the parameters it documents; the rest
// are ignored. `a` is the primary parameter (limit / lo / rate-limit / epsilon /
// spike k), `b` the secondary (OutOfRange hi only), and `window` the Spike
// window length or the StuckAt minimum run length.
struct DetectRule {
  uint32_t id = 0;
  uint16_t channel_id = 0;
  size_t field_index = 0;
  DetectKind kind = DetectKind::ThresholdHigh;
  double a = 0.0;       // primary parameter: limit / lo / rate-limit / epsilon / k
  double b = 0.0;       // secondary: hi (OutOfRange) — unused otherwise
  size_t window = 0;    // Spike window length; StuckAt minimum run length
};

// One firing of a rule at one sample.
struct Detection {
  uint32_t rule_id = 0;
  uint16_t channel_id = 0;
  size_t field_index = 0;
  DetectKind kind = DetectKind::ThresholdHigh;
  size_t sample_index = 0;   // index into the series where it fired
  uint64_t ts = 0;
  double value = 0.0;
  double magnitude = 0.0;    // how far past the condition (kind-specific, >=0)
};

// Evaluates one rule over an ascending-by-timestamp series, appending each
// firing to `out` (existing contents are preserved). Never throws. Semantics per
// kind, with magnitude always >= 0:
//   ThresholdHigh  fires where value > a;    magnitude = value - a.
//   ThresholdLow   fires where value < a;    magnitude = a - value.
//   OutOfRange     fires where value < a or value > b; magnitude = the amount
//                  past the nearer bound (a - value below, value - b above).
//   RateOfChange   for each i >= 1, rate = |value[i]-value[i-1]| / max(dt,1)
//                  where dt = ts[i]-ts[i-1]; fires where rate > a at sample i;
//                  magnitude = rate - a. dt == 0 is treated as 1 (no divide by
//                  zero); a non-ascending pair is treated as dt == 0.
//   StuckAt        a run is a maximal span of consecutive samples whose every
//                  successive delta is <= epsilon (a); the first sample of a run
//                  has run length 1. Fires at every sample whose run length is
//                  >= `window` (the minimum run length); magnitude = that run
//                  length. A change of more than epsilon resets the run. A
//                  `window` of 0 makes StuckAt a no-op.
//   Spike          for each sample i with a full trailing window available
//                  (i.e. i >= window-1), the window is the `window` samples
//                  ending at i (inclusive). Computes that window's mean and
//                  sample standard deviation (N-1 denominator, two-pass). Fires
//                  where |value[i] - mean| > a * stddev; magnitude =
//                  |value[i] - mean| - a * stddev. A stddev of 0 never fires. A
//                  window < 2 or larger than the series makes Spike a no-op.
void evaluate_rule(const std::vector<std::pair<uint64_t, double>>& series,
                   const DetectRule& rule, std::vector<Detection>& out);

// Why a detector-rule line failed to parse.
enum class DetectParseError : uint8_t {
  None,          // no error
  Empty,         // a blank or comment-only line; skipped, never recorded
  BadKind,       // the kind keyword was not one of high/low/range/rate/stuck/spike
  MissingField,  // a required token (kind or a parameter) was absent
  BadNumber,     // a numeric token (channel id or a real/integer parameter) did
                 // not parse
  BadField,      // the field-index token was not a non-negative integer
};

// Human-readable form of a DetectParseError. Never null.
const char* detect_parse_error_str(DetectParseError e);

// The outcome of parsing rule text: the rules from every well-formed line (in
// file order, ids assigned sequentially from 1) plus one (line, error) pair for
// each malformed line. A malformed line never discards the rules around it.
struct DetectRuleParse {
  std::vector<DetectRule> rules;
  std::vector<std::pair<size_t, DetectParseError>> errors;

  bool ok() const { return errors.empty(); }
};

// Parses detector rules from text. Each non-blank, non-comment line is one rule:
//   <channel_id> <field_index> high  <limit>
//   <channel_id> <field_index> low   <limit>
//   <channel_id> <field_index> range <lo> <hi>
//   <channel_id> <field_index> rate  <limit>
//   <channel_id> <field_index> stuck <epsilon> <run_len>
//   <channel_id> <field_index> spike <window> <k>
// <channel_id> and <field_index> accept decimal or 0x-hex; <window>/<run_len>
// are non-negative integers; the remaining parameters are reals. A '#' begins a
// comment; blank and comment-only lines are ignored. Ids are assigned 1, 2, ...
// in file order to the well-formed rules. Every malformed line is recorded with
// its 1-based line number and a specific error code, and parsing continues.
DetectRuleParse parse_detect_rules(const std::string& text);

// ChannelStore adapter: for each rule, resolves its (channel_id, field_index)
// through the store, pulls the field's (timestamp, value) series just as
// aggregate.cpp's column_with_timestamps does (each sample inherits its block's
// base timestamp), evaluates the rule, and accumulates every firing. Detections
// are returned in rule order, then sample order within a rule. A rule naming an
// unknown channel or an out-of-range field contributes no detections.
std::vector<Detection> run_detectors(const ChannelStore& store,
                                     const std::vector<DetectRule>& rules);

}  // namespace blackspan

#endif  // BLACKSPAN_DETECT_H
