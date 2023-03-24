#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/detect.h"
#include "blackspan/pipeline.h"

using namespace blackspan;

static int failures = 0;
#define CHECK(cond)                                             \
  do {                                                          \
    if (!(cond)) {                                              \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, \
                   __FILE__, __LINE__);                         \
      ++failures;                                               \
    }                                                           \
  } while (0)

static bool nearly(double a, double b) { return std::fabs(a - b) < 1e-9; }

using Series = std::vector<std::pair<uint64_t, double>>;

static Series with_unit_ts(const std::vector<double>& values) {
  Series s;
  for (size_t i = 0; i < values.size(); ++i)
    s.emplace_back(static_cast<uint64_t>(i), values[i]);
  return s;
}

static DetectRule rule_of(DetectKind kind, double a, double b = 0.0,
                          size_t window = 0) {
  DetectRule r;
  r.id = 7;
  r.channel_id = 3;
  r.field_index = 1;
  r.kind = kind;
  r.a = a;
  r.b = b;
  r.window = window;
  return r;
}

static void test_kind_names() {
  CHECK(std::string(detect_kind_name(DetectKind::ThresholdHigh)) == "high");
  CHECK(std::string(detect_kind_name(DetectKind::ThresholdLow)) == "low");
  CHECK(std::string(detect_kind_name(DetectKind::OutOfRange)) == "range");
  CHECK(std::string(detect_kind_name(DetectKind::RateOfChange)) == "rate");
  CHECK(std::string(detect_kind_name(DetectKind::StuckAt)) == "stuck");
  CHECK(std::string(detect_kind_name(DetectKind::Spike)) == "spike");
}

static void test_threshold_high() {
  Series s = with_unit_ts({50, 150, 100, 200, 100});
  std::vector<Detection> out;
  evaluate_rule(s, rule_of(DetectKind::ThresholdHigh, 100.0), out);
  CHECK(out.size() == 2);
  if (out.size() == 2) {
    CHECK(out[0].sample_index == 1);
    CHECK(out[0].ts == 1);
    CHECK(nearly(out[0].value, 150));
    CHECK(nearly(out[0].magnitude, 50));
    CHECK(out[0].rule_id == 7 && out[0].channel_id == 3 && out[0].field_index == 1);
    CHECK(out[0].kind == DetectKind::ThresholdHigh);
    CHECK(out[1].sample_index == 3);
    CHECK(nearly(out[1].magnitude, 100));
  }
}

static void test_threshold_low() {
  Series s = with_unit_ts({30, 10, 20, 5});
  std::vector<Detection> out;
  evaluate_rule(s, rule_of(DetectKind::ThresholdLow, 20.0), out);
  CHECK(out.size() == 2);
  if (out.size() == 2) {
    CHECK(out[0].sample_index == 1);
    CHECK(nearly(out[0].magnitude, 10));
    CHECK(out[1].sample_index == 3);
    CHECK(nearly(out[1].magnitude, 15));
  }
}

static void test_out_of_range() {
  Series s = with_unit_ts({5, -3, 10, 15, 0});
  std::vector<Detection> out;

  evaluate_rule(s, rule_of(DetectKind::OutOfRange, 0.0, 10.0), out);
  CHECK(out.size() == 2);
  if (out.size() == 2) {
    CHECK(out[0].sample_index == 1);
    CHECK(nearly(out[0].magnitude, 3));
    CHECK(out[1].sample_index == 3);
    CHECK(nearly(out[1].magnitude, 5));
  }
}

static void test_rate_of_change() {

  Series s = {{0, 0.0}, {10, 5.0}, {20, 5.0}, {20, 30.0}, {40, 31.0}};
  std::vector<Detection> out;
  evaluate_rule(s, rule_of(DetectKind::RateOfChange, 0.4), out);

  CHECK(out.size() == 2);
  if (out.size() == 2) {
    CHECK(out[0].sample_index == 1);
    CHECK(nearly(out[0].magnitude, 0.1));
    CHECK(out[1].sample_index == 3);
    CHECK(out[1].ts == 20);
    CHECK(nearly(out[1].magnitude, 24.6));
  }
}

static void test_stuck_at() {

  Series s = with_unit_ts({1.0, 1.0, 1.0, 1.0, 5.0, 5.0, 5.0});

  std::vector<Detection> out;
  evaluate_rule(s, rule_of(DetectKind::StuckAt, 0.5, 0.0, 3), out);
  CHECK(out.size() == 3);
  if (out.size() == 3) {
    CHECK(out[0].sample_index == 2 && nearly(out[0].magnitude, 3));
    CHECK(out[1].sample_index == 3 && nearly(out[1].magnitude, 4));
    CHECK(out[2].sample_index == 6 && nearly(out[2].magnitude, 3));
  }

  std::vector<Detection> none;
  evaluate_rule(s, rule_of(DetectKind::StuckAt, 0.5, 0.0, 0), none);
  CHECK(none.empty());

  Series changing = with_unit_ts({0, 10, 20, 30, 40});
  std::vector<Detection> out2;
  evaluate_rule(changing, rule_of(DetectKind::StuckAt, 0.5, 0.0, 2), out2);
  CHECK(out2.empty());
}

static void test_spike() {

  Series s = with_unit_ts({10, 10, 10, 10, 10, 100, 10, 10, 10});
  std::vector<Detection> out;
  evaluate_rule(s, rule_of(DetectKind::Spike, 1.0, 0.0, 4), out);

  CHECK(out.size() == 1);
  if (out.size() == 1) {
    CHECK(out[0].sample_index == 5);
    CHECK(out[0].ts == 5);
    CHECK(nearly(out[0].value, 100));
    CHECK(nearly(out[0].magnitude, 22.5));
  }

  Series flat = with_unit_ts({7, 7, 7, 7, 7, 7});
  std::vector<Detection> none;
  evaluate_rule(flat, rule_of(DetectKind::Spike, 1.0, 0.0, 3), none);
  CHECK(none.empty());

  std::vector<Detection> big;
  evaluate_rule(s, rule_of(DetectKind::Spike, 1.0, 0.0, s.size() + 1), big);
  CHECK(big.empty());
  std::vector<Detection> zero;
  evaluate_rule(s, rule_of(DetectKind::Spike, 1.0, 0.0, 0), zero);
  CHECK(zero.empty());
  std::vector<Detection> one;
  evaluate_rule(s, rule_of(DetectKind::Spike, 1.0, 0.0, 1), one);
  CHECK(one.empty());
}

static void test_spike_window_boundary_fire() {

  Series s = with_unit_ts({10, 10, 40});
  std::vector<Detection> out;
  evaluate_rule(s, rule_of(DetectKind::Spike, 1.0, 0.0, 3), out);

  CHECK(out.size() == 1);
  if (out.size() == 1) {
    CHECK(out[0].sample_index == 2);
    CHECK(nearly(out[0].magnitude, 20.0 - std::sqrt(300.0)));
  }
}

static void test_parse_multi_and_errors() {
  const char* text =
      "# detectors\n"
      "1 0 high 100\n"
      "2 1 range -5 5\n"
      "\n"
      "3 0 stuck 0.01 4\n"
      "1 0 wobble 5\n"
      "1 0 high\n"
      "1 0 high abc\n"
      "1 zz high 5\n"
      "4 2 spike 8 3.0\n";
  DetectRuleParse p = parse_detect_rules(text);

  CHECK(p.rules.size() == 4);
  if (p.rules.size() == 4) {
    CHECK(p.rules[0].id == 1 && p.rules[0].kind == DetectKind::ThresholdHigh);
    CHECK(p.rules[0].channel_id == 1 && p.rules[0].field_index == 0);
    CHECK(nearly(p.rules[0].a, 100));

    CHECK(p.rules[1].id == 2 && p.rules[1].kind == DetectKind::OutOfRange);
    CHECK(p.rules[1].channel_id == 2 && p.rules[1].field_index == 1);
    CHECK(nearly(p.rules[1].a, -5) && nearly(p.rules[1].b, 5));

    CHECK(p.rules[2].id == 3 && p.rules[2].kind == DetectKind::StuckAt);
    CHECK(nearly(p.rules[2].a, 0.01) && p.rules[2].window == 4);

    CHECK(p.rules[3].id == 4 && p.rules[3].kind == DetectKind::Spike);
    CHECK(p.rules[3].channel_id == 4 && p.rules[3].field_index == 2);
    CHECK(p.rules[3].window == 8 && nearly(p.rules[3].a, 3.0));
  }

  CHECK(p.errors.size() == 4);
  if (p.errors.size() == 4) {
    CHECK(p.errors[0].first == 6 && p.errors[0].second == DetectParseError::BadKind);
    CHECK(p.errors[1].first == 7 && p.errors[1].second == DetectParseError::MissingField);
    CHECK(p.errors[2].first == 8 && p.errors[2].second == DetectParseError::BadNumber);
    CHECK(p.errors[3].first == 9 && p.errors[3].second == DetectParseError::BadField);
  }
  CHECK(!p.ok());
}

static void test_parse_clean() {
  DetectRuleParse p = parse_detect_rules(
      "5 0 low 1.5\n"
      "6 3 rate 0.25\n");
  CHECK(p.ok());
  CHECK(p.rules.size() == 2);
  if (p.rules.size() == 2) {
    CHECK(p.rules[0].kind == DetectKind::ThresholdLow && nearly(p.rules[0].a, 1.5));
    CHECK(p.rules[1].kind == DetectKind::RateOfChange && nearly(p.rules[1].a, 0.25));
    CHECK(p.rules[1].channel_id == 6 && p.rules[1].field_index == 3);
  }
}

static void test_error_strings() {
  CHECK(std::string(detect_parse_error_str(DetectParseError::None)) == "none");
  CHECK(std::string(detect_parse_error_str(DetectParseError::BadKind)) == "bad-kind");
  CHECK(std::string(detect_parse_error_str(DetectParseError::BadField)) == "bad-field");
  CHECK(std::string(detect_parse_error_str(DetectParseError::BadNumber)) == "bad-number");
}

static void test_adapter_from_recording() {

  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"
      "  row 10\n"
      "  row 20\n"
      "end\n"
      "samples channel=1 delta=0 ts=1100\n"
      "  row 30\n"
      "  row 40\n"
      "end\n"
      "session-end reason=0\n";
  AssembleResult a = assemble_script(script);
  for (const AssembleDiagnostic& d : a.errors) {
    std::fprintf(stderr, "assemble: %s\n",
                 describe_assemble_diagnostic(d).c_str());
  }
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());

  DetectRuleParse rp = parse_detect_rules("1 0 high 25\n");
  CHECK(rp.ok());
  std::vector<Detection> hits = run_detectors(rec.channels(), rp.rules);
  CHECK(hits.size() == 2);
  if (hits.size() == 2) {
    CHECK(hits[0].sample_index == 2 && hits[0].ts == 1100);
    CHECK(nearly(hits[0].value, 30) && nearly(hits[0].magnitude, 5));
    CHECK(hits[1].sample_index == 3 && hits[1].ts == 1100);
    CHECK(nearly(hits[1].value, 40) && nearly(hits[1].magnitude, 15));
    CHECK(hits[0].channel_id == 1 && hits[0].field_index == 0);
    CHECK(hits[0].rule_id == 1);
  }

  DetectRuleParse rp2 = parse_detect_rules(
      "9 0 high 0\n"
      "1 5 high 0\n");
  std::vector<Detection> none = run_detectors(rec.channels(), rp2.rules);
  CHECK(none.empty());
}

int main() {
  test_kind_names();
  test_threshold_high();
  test_threshold_low();
  test_out_of_range();
  test_rate_of_change();
  test_stuck_at();
  test_spike();
  test_spike_window_boundary_fire();
  test_parse_multi_and_errors();
  test_parse_clean();
  test_error_strings();
  test_adapter_from_recording();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_detect: all checks passed\n");
  return 0;
}
