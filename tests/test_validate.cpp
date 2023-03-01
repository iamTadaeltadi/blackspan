#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/pipeline.h"
#include "blackspan/validate.h"

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

static bool decode_script(const std::string& script, std::vector<uint8_t>& bytes,
                          DecodedRecording& rec) {
  AssembleResult r = assemble_script(script);
  for (const AssembleDiagnostic& d : r.errors) {
    std::fprintf(stderr, "assemble: %s\n",
                 describe_assemble_diagnostic(d).c_str());
  }
  if (!r.ok()) {
    ++failures;
    return false;
  }
  bytes = r.bytes;
  rec.decode(bytes.data(), bytes.size());
  return true;
}

static bool has(const ValidationReport& r, ValidationCode c) {
  return r.count_of(c) > 0;
}

// A clean, well-formed recording yields no errors or warnings.
static void test_clean() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 100\n"
      "tick 10\n"
      "session-begin label=ok\n"
      "schema id=1 name=m fields=v:u16\n"
      "state from=0 to=1 cause=1\n"
      "samples channel=1 delta=0\n"
      "  row 5\n"
      "end\n"
      "marker text=note\n"
      "session-end reason=0\n";
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(script, bytes, rec)) return;

  ValidationReport report = ValidationReport::run(rec);
  CHECK(report.summary().errors == 0);
  CHECK(report.summary().warnings == 0);
  CHECK(report.summary().worst() == Severity3::Note ||
        report.summary().total() == 0);
  // A declared-and-sampled channel is not flagged unused.
  CHECK(!has(report, ValidationCode::ChannelDeclaredUnused));
}

static void test_empty_recording() {
  std::vector<uint8_t> empty;
  DecodedRecording rec;
  rec.decode(empty.data(), empty.size());
  ValidationReport report = ValidationReport::run(rec);
  CHECK(has(report, ValidationCode::EmptyRecording));
  CHECK(report.summary().warnings >= 1);
}

static void test_truncated_and_halted() {
  // Truncated: cut the last frame short -> Incomplete -> a warning.
  {
    const char* script =
        "device 1\n"
        "session 1\n"
        "marker text=a\n"
        "marker text=b\n"
        "damage mode=truncate bytes=5\n";
    std::vector<uint8_t> bytes;
    DecodedRecording rec;
    if (!decode_script(script, bytes, rec)) return;
    ValidationReport report = ValidationReport::run(rec);
    CHECK(has(report, ValidationCode::StreamTruncated));
    CHECK(!has(report, ValidationCode::StreamHalted));
  }
  // Halted: a hard frame error mid-stream -> an error.
  {
    const char* script =
        "device 1\n"
        "session 1\n"
        "marker text=a\n"
        "marker text=b\n"
        "damage mode=magic\n";
    std::vector<uint8_t> bytes;
    DecodedRecording rec;
    if (!decode_script(script, bytes, rec)) return;
    ValidationReport report = ValidationReport::run(rec);
    CHECK(has(report, ValidationCode::StreamHalted));
    CHECK(report.summary().errors >= 1);
    CHECK(report.summary().worst() == Severity3::Error);
  }
}

static void test_unterminated_and_end_without_begin() {
  const char* script =
      "device 1\n"
      "session 5\n"
      "ts 100\n"
      "session-end reason=0\n"     // end with no begin
      "session-begin label=open\n"  // never closed
      "marker text=x\n";
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(script, bytes, rec)) return;
  ValidationReport report = ValidationReport::run(rec);
  CHECK(has(report, ValidationCode::EndWithoutBegin));
  CHECK(has(report, ValidationCode::UnterminatedSession));
  CHECK(report.summary().errors >= 1);  // end-without-begin is an error
}

static void test_regression_flags() {
  // A frame whose ts and seq run backwards within a session.
  const char* script =
      "device 1\n"
      "session 10\n"
      "ts 1000\n"
      "session-begin label=r\n"
      "marker text=a seq=5 ts=500\n"  // seq and ts both go backwards
      "session-end reason=0 seq=11 ts=1100\n";
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(script, bytes, rec)) return;
  ValidationReport report = ValidationReport::run(rec);
  CHECK(has(report, ValidationCode::SessionSeqRegression));
  CHECK(has(report, ValidationCode::SessionTsRegression));
}

static void test_channel_declared_unused() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "session-begin label=u\n"
      "schema id=1 name=used fields=v:u8\n"
      "schema id=2 name=unused fields=v:u8\n"
      "samples channel=1\n"
      "  row 1\n"
      "end\n"
      "session-end reason=0\n";
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(script, bytes, rec)) return;
  ValidationReport report = ValidationReport::run(rec);
  CHECK(has(report, ValidationCode::ChannelDeclaredUnused));
  // Only channel 2 is unused.
  CHECK(report.count_of(ValidationCode::ChannelDeclaredUnused) == 1);
}

static void test_anomaly_notes() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 100\n"
      "tick 10\n"
      "session-begin label=a\n"
      // A fault before any transition -> fault-without-state note.
      "fault code=1 severity=warning\n"
      "state from=0 to=1 cause=1\n"
      // A desync: from_state does not continue the chain.
      "state from=9 to=2 cause=1\n"
      // A critical fault.
      "fault code=99 severity=critical\n"
      "session-end reason=0\n";
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(script, bytes, rec)) return;

  ValidationReport report = ValidationReport::run(rec);
  CHECK(has(report, ValidationCode::FaultWithoutState));
  CHECK(has(report, ValidationCode::StateDesync));
  CHECK(has(report, ValidationCode::CriticalFault));

  // With notes suppressed those disappear but warnings/errors remain.
  ValidationOptions no_notes;
  no_notes.include_notes = false;
  ValidationReport quiet = ValidationReport::run(rec, no_notes);
  CHECK(!has(quiet, ValidationCode::FaultWithoutState));
  CHECK(!has(quiet, ValidationCode::StateDesync));
  CHECK(!has(quiet, ValidationCode::CriticalFault));
  CHECK(quiet.summary().notes == 0);
}

static void test_fault_burst() {
  std::string script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "tick 10\n"
      "session-begin label=b\n"
      "state from=0 to=1 cause=1\n";
  for (int i = 0; i < 5; ++i) script += "fault code=1 severity=error\n";
  script += "session-end reason=0\n";

  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(script, bytes, rec)) return;

  ValidationOptions opts;
  opts.burst_window_ns = 1000;
  opts.burst_min_count = 3;
  ValidationReport report = ValidationReport::run(rec, opts);
  CHECK(has(report, ValidationCode::FaultBurst));
}

static void test_render_and_severity_order() {
  std::vector<uint8_t> empty;
  DecodedRecording rec;
  rec.decode(empty.data(), empty.size());
  ValidationReport report = ValidationReport::run(rec);
  std::string a = render_validation(report);
  std::string b = render_validation(report);
  CHECK(a == b);
  CHECK(a.find("empty-recording") != std::string::npos);

  // A clean recording renders the ok line.
  const char* clean =
      "device 1\n"
      "session 1\n"
      "session-begin label=c\n"
      "state from=0 to=1 cause=1\n"
      "session-end reason=0\n";
  std::vector<uint8_t> bytes;
  DecodedRecording rec2;
  if (!decode_script(clean, bytes, rec2)) return;
  ValidationReport r2 = ValidationReport::run(rec2);
  if (r2.findings().empty()) {
    CHECK(render_validation(r2) == "ok: no findings\n");
  }

  // at_severity returns only the requested tier.
  for (const Finding* f : report.at_severity(Severity3::Warning)) {
    CHECK(f->severity == Severity3::Warning);
  }
}

int main() {
  test_clean();
  test_empty_recording();
  test_truncated_and_halted();
  test_unterminated_and_end_without_begin();
  test_regression_flags();
  test_channel_declared_unused();
  test_anomaly_notes();
  test_fault_burst();
  test_render_and_severity_order();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_validate: all checks passed\n");
  return 0;
}
