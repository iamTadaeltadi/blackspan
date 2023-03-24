#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "blackspan/detect.h"

namespace blackspan {
namespace {

bool parse_uint(const std::string& s, uint64_t& out) {
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  unsigned long long v = std::strtoull(s.c_str(), &end, 0);
  if (end == s.c_str() || *end != '\0') return false;
  out = v;
  return true;
}

bool parse_double_token(const std::string& s, double& out) {
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str()) return false;
  while (*end == ' ' || *end == '\t') ++end;
  if (*end != '\0') return false;
  out = v;
  return true;
}

bool parse_detect_kind(const std::string& s, DetectKind& out) {
  if (s == "high") { out = DetectKind::ThresholdHigh; return true; }
  if (s == "low") { out = DetectKind::ThresholdLow; return true; }
  if (s == "range") { out = DetectKind::OutOfRange; return true; }
  if (s == "rate") { out = DetectKind::RateOfChange; return true; }
  if (s == "stuck") { out = DetectKind::StuckAt; return true; }
  if (s == "spike") { out = DetectKind::Spike; return true; }
  return false;
}

Detection make_detection(const DetectRule& rule) {
  Detection d;
  d.rule_id = rule.id;
  d.channel_id = rule.channel_id;
  d.field_index = rule.field_index;
  d.kind = rule.kind;
  return d;
}

void eval_threshold_high(const std::vector<std::pair<uint64_t, double>>& series,
                         const DetectRule& rule, std::vector<Detection>& out) {
  for (size_t i = 0; i < series.size(); ++i) {
    double v = series[i].second;
    if (v > rule.a) {
      Detection d = make_detection(rule);
      d.sample_index = i;
      d.ts = series[i].first;
      d.value = v;
      d.magnitude = v - rule.a;
      out.push_back(d);
    }
  }
}

void eval_threshold_low(const std::vector<std::pair<uint64_t, double>>& series,
                        const DetectRule& rule, std::vector<Detection>& out) {
  for (size_t i = 0; i < series.size(); ++i) {
    double v = series[i].second;
    if (v < rule.a) {
      Detection d = make_detection(rule);
      d.sample_index = i;
      d.ts = series[i].first;
      d.value = v;
      d.magnitude = rule.a - v;
      out.push_back(d);
    }
  }
}

void eval_out_of_range(const std::vector<std::pair<uint64_t, double>>& series,
                       const DetectRule& rule, std::vector<Detection>& out) {
  const double lo = rule.a;
  const double hi = rule.b;
  for (size_t i = 0; i < series.size(); ++i) {
    double v = series[i].second;
    if (v < lo || v > hi) {
      Detection d = make_detection(rule);
      d.sample_index = i;
      d.ts = series[i].first;
      d.value = v;
      d.magnitude = (v < lo) ? (lo - v) : (v - hi);
      out.push_back(d);
    }
  }
}

void eval_rate_of_change(const std::vector<std::pair<uint64_t, double>>& series,
                         const DetectRule& rule, std::vector<Detection>& out) {
  for (size_t i = 1; i < series.size(); ++i) {
    uint64_t t_prev = series[i - 1].first;
    uint64_t t_cur = series[i].first;

    uint64_t dt = (t_cur >= t_prev) ? (t_cur - t_prev) : 0;
    double denom = (dt == 0) ? 1.0 : static_cast<double>(dt);
    double rate = std::fabs(series[i].second - series[i - 1].second) / denom;
    if (rate > rule.a) {
      Detection d = make_detection(rule);
      d.sample_index = i;
      d.ts = t_cur;
      d.value = series[i].second;
      d.magnitude = rate - rule.a;
      out.push_back(d);
    }
  }
}

void eval_stuck_at(const std::vector<std::pair<uint64_t, double>>& series,
                   const DetectRule& rule, std::vector<Detection>& out) {
  const size_t run_len = rule.window;
  if (run_len == 0 || series.empty()) return;
  const double epsilon = rule.a;

  size_t run = 0;
  for (size_t i = 0; i < series.size(); ++i) {
    if (i == 0 ||
        std::fabs(series[i].second - series[i - 1].second) <= epsilon) {
      ++run;
    } else {
      run = 1;
    }
    if (run >= run_len) {
      Detection d = make_detection(rule);
      d.sample_index = i;
      d.ts = series[i].first;
      d.value = series[i].second;
      d.magnitude = static_cast<double>(run);
      out.push_back(d);
    }
  }
}

void eval_spike(const std::vector<std::pair<uint64_t, double>>& series,
                const DetectRule& rule, std::vector<Detection>& out) {
  const size_t w = rule.window;

  if (w < 2 || w > series.size()) return;
  const double k = rule.a;

  for (size_t i = w - 1; i < series.size(); ++i) {
    const size_t begin = i + 1 - w;

    double sum = 0.0;
    for (size_t j = begin; j <= i; ++j) sum += series[j].second;
    double mean = sum / static_cast<double>(w);

    double sq = 0.0;
    for (size_t j = begin; j <= i; ++j) {
      double dv = series[j].second - mean;
      sq += dv * dv;
    }
    double stddev = std::sqrt(sq / static_cast<double>(w - 1));
    if (!(stddev > 0.0)) continue;

    double dev = std::fabs(series[i].second - mean);
    if (dev > k * stddev) {
      Detection d = make_detection(rule);
      d.sample_index = i;
      d.ts = series[i].first;
      d.value = series[i].second;
      d.magnitude = dev - k * stddev;
      out.push_back(d);
    }
  }
}

}

const char* detect_kind_name(DetectKind k) {
  switch (k) {
    case DetectKind::ThresholdHigh: return "high";
    case DetectKind::ThresholdLow: return "low";
    case DetectKind::OutOfRange: return "range";
    case DetectKind::RateOfChange: return "rate";
    case DetectKind::StuckAt: return "stuck";
    case DetectKind::Spike: return "spike";
  }
  return "unknown";
}

const char* detect_parse_error_str(DetectParseError e) {
  switch (e) {
    case DetectParseError::None: return "none";
    case DetectParseError::Empty: return "empty";
    case DetectParseError::BadKind: return "bad-kind";
    case DetectParseError::MissingField: return "missing-field";
    case DetectParseError::BadNumber: return "bad-number";
    case DetectParseError::BadField: return "bad-field";
  }
  return "unknown";
}

void evaluate_rule(const std::vector<std::pair<uint64_t, double>>& series,
                   const DetectRule& rule, std::vector<Detection>& out) {
  switch (rule.kind) {
    case DetectKind::ThresholdHigh: eval_threshold_high(series, rule, out); break;
    case DetectKind::ThresholdLow: eval_threshold_low(series, rule, out); break;
    case DetectKind::OutOfRange: eval_out_of_range(series, rule, out); break;
    case DetectKind::RateOfChange: eval_rate_of_change(series, rule, out); break;
    case DetectKind::StuckAt: eval_stuck_at(series, rule, out); break;
    case DetectKind::Spike: eval_spike(series, rule, out); break;
  }
}

DetectRuleParse parse_detect_rules(const std::string& text) {
  DetectRuleParse result;
  uint32_t next_id = 1;

  size_t line_no = 0;
  size_t start = 0;
  while (start <= text.size()) {
    ++line_no;
    size_t nl = text.find('\n', start);
    size_t end = (nl == std::string::npos) ? text.size() : nl;
    std::string line = text.substr(start, end - start);
    if (nl == std::string::npos)
      start = text.size() + 1;
    else
      start = nl + 1;

    size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);

    std::vector<std::string> tok;
    {
      std::istringstream iss(line);
      std::string t;
      while (iss >> t) tok.push_back(t);
    }
    if (tok.empty()) continue;

    auto fail = [&](DetectParseError e) {
      result.errors.emplace_back(line_no, e);
    };

    if (tok.size() < 3) {
      fail(DetectParseError::MissingField);
      continue;
    }

    uint64_t channel_id = 0;
    if (!parse_uint(tok[0], channel_id) || channel_id > 0xffff) {
      fail(DetectParseError::BadNumber);
      continue;
    }
    uint64_t field_index = 0;
    if (!parse_uint(tok[1], field_index)) {
      fail(DetectParseError::BadField);
      continue;
    }
    DetectKind kind;
    if (!parse_detect_kind(tok[2], kind)) {
      fail(DetectParseError::BadKind);
      continue;
    }

    DetectRule rule;
    rule.channel_id = static_cast<uint16_t>(channel_id);
    rule.field_index = static_cast<size_t>(field_index);
    rule.kind = kind;

    bool bad = false;
    switch (kind) {
      case DetectKind::ThresholdHigh:
      case DetectKind::ThresholdLow:
      case DetectKind::RateOfChange: {
        if (tok.size() < 4) { fail(DetectParseError::MissingField); bad = true; break; }
        if (!parse_double_token(tok[3], rule.a)) { fail(DetectParseError::BadNumber); bad = true; }
        break;
      }
      case DetectKind::OutOfRange: {
        if (tok.size() < 5) { fail(DetectParseError::MissingField); bad = true; break; }
        if (!parse_double_token(tok[3], rule.a) ||
            !parse_double_token(tok[4], rule.b)) {
          fail(DetectParseError::BadNumber);
          bad = true;
        }
        break;
      }
      case DetectKind::StuckAt: {
        if (tok.size() < 5) { fail(DetectParseError::MissingField); bad = true; break; }
        uint64_t run_len = 0;
        if (!parse_double_token(tok[3], rule.a) || !parse_uint(tok[4], run_len)) {
          fail(DetectParseError::BadNumber);
          bad = true;
          break;
        }
        rule.window = static_cast<size_t>(run_len);
        break;
      }
      case DetectKind::Spike: {
        if (tok.size() < 5) { fail(DetectParseError::MissingField); bad = true; break; }
        uint64_t window = 0;
        if (!parse_uint(tok[3], window) || !parse_double_token(tok[4], rule.a)) {
          fail(DetectParseError::BadNumber);
          bad = true;
          break;
        }
        rule.window = static_cast<size_t>(window);
        break;
      }
    }
    if (bad) continue;

    rule.id = next_id++;
    result.rules.push_back(rule);
  }

  return result;
}

std::vector<Detection> run_detectors(const ChannelStore& store,
                                     const std::vector<DetectRule>& rules) {
  std::vector<Detection> out;
  for (const DetectRule& rule : rules) {
    const ChannelSeries* series = store.series(rule.channel_id);
    if (series == nullptr) continue;
    if (rule.field_index >= series->schema().fields.size()) continue;

    std::vector<std::pair<uint64_t, double>> pairs;
    size_t global = 0;
    for (size_t b = 0; b < series->block_count(); ++b) {
      uint64_t base_ts = series->block_base_ts(b);
      uint16_t n = series->block_sample_count(b);
      for (uint16_t i = 0; i < n; ++i, ++global) {
        auto view = series->view(global);
        if (!view.has_value()) continue;
        auto value = view->get_as_double(rule.field_index);
        if (!value.has_value()) continue;
        pairs.emplace_back(base_ts, value.value());
      }
    }
    evaluate_rule(pairs, rule, out);
  }
  return out;
}

}
