#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/calibrate.h"
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

static void test_rule_apply() {
  CalibrationRule r;
  r.scale = 0.01;
  r.offset = -5.0;
  CHECK(nearly(r.apply(1000), 5.0));
  CHECK(nearly(r.apply(0), -5.0));

  CalibrationRule identity;
  CHECK(nearly(identity.apply(42), 42));
}

static void test_table_set_get() {
  CalibrationTable t;
  CalibrationRule r;
  r.channel_id = 1;
  r.field_index = 0;
  r.scale = 2.0;
  r.offset = 1.0;
  r.unit = "V";
  t.set(r);
  CHECK(t.size() == 1);
  CHECK(t.get(1, 0) != nullptr);
  CHECK(t.get(1, 1) == nullptr);
  CHECK(nearly(t.apply(1, 0, 3), 7.0));
  CHECK(nearly(t.apply(1, 1, 3), 3.0));
  CHECK(nearly(t.apply(9, 0, 3), 3.0));

  r.scale = 3.0;
  t.set(r);
  CHECK(t.size() == 1);
  CHECK(nearly(t.apply(1, 0, 1), 4.0));
}

static void test_parse_basic() {
  const char* text =
      "# site calibration\n"
      "channel 1 field 0 scale 0.1 offset -2 unit volts\n"
      "channel 1 field 1 scale 1 offset 0 unit amps\n"
      "channel 2 field 0 scale 0.25 offset 0\n";
  CalibrationParse p = parse_calibration(text);
  for (const CalibrationDiagnostic& d : p.errors) {
    std::fprintf(stderr, "cal: line %zu %s (%s)\n", d.line,
                 calibration_error_str(d.error), d.detail.c_str());
  }
  CHECK(p.ok());
  CHECK(p.table.size() == 3);
  const CalibrationRule* r = p.table.get(1, 0);
  CHECK(r != nullptr);
  if (r) {
    CHECK(nearly(r->scale, 0.1));
    CHECK(nearly(r->offset, -2));
    CHECK(r->unit == "volts");
  }
  CHECK(p.table.get(2, 0) != nullptr);
  CHECK(p.table.get(2, 0)->unit.empty());
}

static void test_parse_keyword_order_and_defaults() {

  const char* text =
      "channel 3 unit deg field 2 offset 10\n";
  CalibrationParse p = parse_calibration(text);
  CHECK(p.ok());
  const CalibrationRule* r = p.table.get(3, 2);
  CHECK(r != nullptr);
  if (r) {
    CHECK(nearly(r->scale, 1.0));
    CHECK(nearly(r->offset, 10.0));
    CHECK(r->unit == "deg");
  }
}

static void test_parse_errors() {
  {
    CalibrationParse p = parse_calibration("bogus 1 field 0\n");
    CHECK(!p.ok());
    CHECK(p.errors[0].error == CalibrationError::UnknownDirective);
  }
  {
    CalibrationParse p = parse_calibration("channel 1 scale 2\n");
    CHECK(!p.ok());
    CHECK(p.errors[0].error == CalibrationError::MissingField);
  }
  {
    CalibrationParse p = parse_calibration("channel 1 field 0 scale xx\n");
    CHECK(!p.ok());
    CHECK(p.errors[0].error == CalibrationError::BadNumber);
  }
  {
    CalibrationParse p = parse_calibration(
        "channel 1 field 0 scale 1\n"
        "channel 1 field 0 scale 2\n");
    CHECK(!p.ok());
    CHECK(p.errors[0].error == CalibrationError::DuplicateRule);
    CHECK(p.errors[0].line == 2);
  }

  {
    CalibrationParse p = parse_calibration(
        "channel 1 field 0 scale 2 offset 0\n"
        "garbage line\n"
        "channel 2 field 0 scale 3 offset 0\n");
    CHECK(!p.ok());
    CHECK(p.table.size() == 2);
    CHECK(p.errors.size() == 1);
    CHECK(p.errors[0].line == 2);
  }
}

static void test_roundtrip() {
  const char* text =
      "channel 1 field 0 scale 0.1 offset -2 unit volts\n"
      "channel 2 field 3 scale 0.25 offset 0\n";
  CalibrationParse p = parse_calibration(text);
  CHECK(p.ok());
  std::string serialized = p.table.to_text();
  CalibrationParse p2 = parse_calibration(serialized);
  CHECK(p2.ok());
  CHECK(p2.table.size() == p.table.size());
  const CalibrationRule* a = p.table.get(1, 0);
  const CalibrationRule* b = p2.table.get(1, 0);
  CHECK(a && b);
  if (a && b) {
    CHECK(nearly(a->scale, b->scale));
    CHECK(nearly(a->offset, b->offset));
    CHECK(a->unit == b->unit);
  }
}

static void test_apply_column() {

  const char* script =
      "device 1\n"
      "session 1\n"
      "session-begin label=a\n"
      "schema id=1 name=bus fields=raw:u16\n"
      "samples channel=1 delta=0\n"
      "  row 1000\n"
      "  row 2000\n"
      "  row 3000\n"
      "end\n"
      "session-end reason=0\n";
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  DecodedRecording rec;
  rec.decode(a.bytes.data(), a.bytes.size());

  CalibrationParse p =
      parse_calibration("channel 1 field 0 scale 0.001 offset 0 unit V\n");
  CHECK(p.ok());
  std::string unit;
  std::vector<double> volts = p.table.apply_column(rec.channels(), 1, 0, &unit);
  CHECK(unit == "V");
  CHECK(volts.size() == 3);
  if (volts.size() == 3) {
    CHECK(nearly(volts[0], 1.0));
    CHECK(nearly(volts[1], 2.0));
    CHECK(nearly(volts[2], 3.0));
  }

  CalibrationTable empty;
  std::string u2;
  std::vector<double> raw = empty.apply_column(rec.channels(), 1, 0, &u2);
  CHECK(u2.empty());
  CHECK(raw.size() == 3);
  if (raw.size() == 3) CHECK(nearly(raw[0], 1000));
}

int main() {
  test_rule_apply();
  test_table_set_get();
  test_parse_basic();
  test_parse_keyword_order_and_defaults();
  test_parse_errors();
  test_roundtrip();
  test_apply_column();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_calibrate: all checks passed\n");
  return 0;
}
