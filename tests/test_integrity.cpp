#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/integrity.h"
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

static bool assemble_ok(const std::string& script, std::vector<uint8_t>& bytes) {
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
  return true;
}

static const IntegrityFinding* find(const IntegrityReport& r, IntegrityCode c) {
  for (const IntegrityFinding& f : r.findings) {
    if (f.code == c) return &f;
  }
  return nullptr;
}

static bool has(const IntegrityReport& r, IntegrityCode c) {
  return find(r, c) != nullptr;
}

static size_t count(const IntegrityReport& r, IntegrityCode c) {
  size_t n = 0;
  for (const IntegrityFinding& f : r.findings) {
    if (f.code == c) ++n;
  }
  return n;
}

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
  if (!assemble_ok(script, bytes)) return;

  IntegrityReport frames = audit_frames(bytes.data(), bytes.size());
  CHECK(frames.clean());
  CHECK(frames.error_count == 0);
  CHECK(frames.warning_count == 0);
  CHECK(frames.frames_total == 6);
  CHECK(frames.frames_ok == 6);
  for (const IntegrityFinding& f : frames.findings) {
    CHECK(f.level == IntegrityLevel::Info);
  }

  DecodedRecording rec;
  rec.decode(bytes.data(), bytes.size());
  IntegrityReport model = audit_recording(rec, bytes.data(), bytes.size());
  CHECK(model.clean());
  CHECK(!has(model, IntegrityCode::SampleWithoutSchema));
  CHECK(!has(model, IntegrityCode::SessionUnterminated));
}

static void test_seq_regression() {
  const char* script =
      "device 7\n"
      "session 20\n"
      "ts 1000\n"
      "session-begin label=r\n"
      "marker text=a seq=5 ts=500\n"
      "session-end reason=0 seq=21 ts=1100\n";
  std::vector<uint8_t> bytes;
  if (!assemble_ok(script, bytes)) return;

  IntegrityReport r = audit_frames(bytes.data(), bytes.size());
  const IntegrityFinding* f = find(r, IntegrityCode::NonMonotonicSeq);
  CHECK(f != nullptr);
  if (f) {
    CHECK(f->device_id == 7);
    CHECK(f->level == IntegrityLevel::Warning);
  }
  CHECK(has(r, IntegrityCode::NonMonotonicTs));
  CHECK(!r.clean());
}

static void test_sample_without_schema() {
  const char* script =
      "device 3\n"
      "session 1\n"
      "raw type=4 hex=070001000000000000000000\n";
  std::vector<uint8_t> bytes;
  if (!assemble_ok(script, bytes)) return;

  IntegrityReport r = audit_frames(bytes.data(), bytes.size());
  const IntegrityFinding* f = find(r, IntegrityCode::SampleWithoutSchema);
  CHECK(f != nullptr);
  if (f) {
    CHECK(f->device_id == 3);
    CHECK(f->level == IntegrityLevel::Error);
  }
  CHECK(r.error_count >= 1);
}

static void test_session_structure() {
  const char* script =
      "device 1\n"
      "session 5\n"
      "ts 100\n"
      "session-end reason=0\n"
      "session-begin label=open\n"
      "marker text=x\n";
  std::vector<uint8_t> bytes;
  if (!assemble_ok(script, bytes)) return;

  DecodedRecording rec;
  rec.decode(bytes.data(), bytes.size());
  IntegrityReport r = audit_recording(rec, bytes.data(), bytes.size());
  CHECK(has(r, IntegrityCode::SessionEndWithoutBegin));
  CHECK(has(r, IntegrityCode::SessionUnterminated));
  CHECK(count(r, IntegrityCode::SessionEndWithoutBegin) == 1);
  CHECK(count(r, IntegrityCode::SessionUnterminated) == 1);
  CHECK(r.error_count >= 1);
}

static void test_session_begin_without_end() {
  const char* script =
      "device 2\n"
      "session 1\n"
      "session-begin label=solo\n"
      "marker text=a\n";
  std::vector<uint8_t> bytes;
  if (!assemble_ok(script, bytes)) return;

  IntegrityReport r = audit_frames(bytes.data(), bytes.size());
  const IntegrityFinding* f = find(r, IntegrityCode::SessionUnterminated);
  CHECK(f != nullptr);
  if (f) CHECK(f->device_id == 2);
  CHECK(!has(r, IntegrityCode::SessionEndWithoutBegin));
}

static void test_schema_redefined() {
  const char* script =
      "device 4\n"
      "session 1\n"
      "schema id=9 name=a fields=v:u8\n"
      "schema id=9 name=b fields=w:u16\n";
  std::vector<uint8_t> bytes;
  if (!assemble_ok(script, bytes)) return;

  IntegrityReport r = audit_frames(bytes.data(), bytes.size());
  const IntegrityFinding* f = find(r, IntegrityCode::SchemaRedefined);
  CHECK(f != nullptr);
  if (f) CHECK(f->device_id == 4);
  CHECK(count(r, IntegrityCode::SchemaRedefined) == 1);
}

static void test_unknown_record_type() {
  const char* script =
      "device 5\n"
      "session 1\n"
      "raw type=32 hex=00\n";
  std::vector<uint8_t> bytes;
  if (!assemble_ok(script, bytes)) return;

  IntegrityReport r = audit_frames(bytes.data(), bytes.size());
  const IntegrityFinding* f = find(r, IntegrityCode::UnknownRecordType);
  CHECK(f != nullptr);
  if (f) CHECK(f->device_id == 5);
  CHECK(r.frames_total == 1);
  CHECK(r.frames_ok == 0);
}

static void test_truncated() {
  const char* script =
      "device 6\n"
      "session 1\n"
      "tick 5\n"
      "marker text=a\n"
      "marker text=b\n"
      "marker text=c\n";
  std::vector<uint8_t> bytes;
  if (!assemble_ok(script, bytes)) return;
  CHECK(bytes.size() > 3);
  bytes.resize(bytes.size() - 3);

  IntegrityReport r = audit_frames(bytes.data(), bytes.size());
  CHECK(has(r, IntegrityCode::TruncatedStream));
  CHECK(r.frames_total >= 1);
  CHECK(!r.clean());
}

static void test_empty() {
  std::vector<uint8_t> empty;
  IntegrityReport r = audit_frames(empty.data(), empty.size());
  CHECK(has(r, IntegrityCode::EmptyRecording));
  CHECK(r.clean());
  CHECK(r.frames_total == 0);
  CHECK(r.frames_ok == 0);
  CHECK(r.error_count == 0);
  CHECK(r.warning_count == 0);

  DecodedRecording rec;
  rec.decode(empty.data(), empty.size());
  IntegrityReport r2 = audit_recording(rec, empty.data(), empty.size());
  CHECK(has(r2, IntegrityCode::EmptyRecording));
  CHECK(r2.clean());
}

static void test_render_and_describe() {
  const char* script =
      "device 8\n"
      "session 1\n"
      "session-begin label=x\n"
      "marker text=y\n";
  std::vector<uint8_t> bytes;
  if (!assemble_ok(script, bytes)) return;

  IntegrityReport r = audit_frames(bytes.data(), bytes.size());
  std::string a = render_integrity(r);
  std::string b = render_integrity(r);
  CHECK(!a.empty());
  CHECK(a == b);
  CHECK(a.find("session-unterminated") != std::string::npos);

  for (const IntegrityFinding& f : r.findings) {
    std::string d = describe_integrity_finding(f);
    CHECK(!d.empty());
    CHECK(d.find(integrity_code_name(f.code)) != std::string::npos);
  }

  std::vector<uint8_t> empty;
  IntegrityReport clean = audit_frames(empty.data(), empty.size());
  std::string rendered = render_integrity(clean);
  CHECK(rendered.find("frames:") != std::string::npos);
}

static void test_ordering_deterministic() {
  const char* script =
      "device 1\n"
      "session 40\n"
      "ts 4000\n"
      "session-begin label=o\n"
      "marker text=a seq=5 ts=500\n"
      "marker text=b seq=6 ts=600\n"
      "session-end reason=0 seq=41 ts=4100\n";
  std::vector<uint8_t> bytes;
  if (!assemble_ok(script, bytes)) return;

  IntegrityReport r1 = audit_frames(bytes.data(), bytes.size());
  IntegrityReport r2 = audit_frames(bytes.data(), bytes.size());
  CHECK(r1.findings.size() == r2.findings.size());
  for (size_t i = 0; i < r1.findings.size(); ++i) {
    CHECK(r1.findings[i].code == r2.findings[i].code);
    CHECK(r1.findings[i].ts == r2.findings[i].ts);
  }
  for (size_t i = 1; i < r1.findings.size(); ++i) {
    CHECK(r1.findings[i - 1].ts <= r1.findings[i].ts);
  }
}

int main() {
  test_clean();
  test_seq_regression();
  test_sample_without_schema();
  test_session_structure();
  test_session_begin_without_end();
  test_schema_redefined();
  test_unknown_record_type();
  test_truncated();
  test_empty();
  test_render_and_describe();
  test_ordering_deterministic();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_integrity: all checks passed\n");
  return 0;
}
