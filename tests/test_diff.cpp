#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/diff.h"
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

struct Decoded {
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
};

static bool load(const std::string& script, Decoded& out) {
  AssembleResult r = assemble_script(script);
  for (const AssembleDiagnostic& d : r.errors) {
    std::fprintf(stderr, "assemble: %s\n",
                 describe_assemble_diagnostic(d).c_str());
  }
  if (!r.ok()) {
    ++failures;
    return false;
  }
  out.bytes = r.bytes;
  out.rec.decode(out.bytes.data(), out.bytes.size());
  return true;
}

static bool has_key(const RecordingDiff& d, const std::string& key) {
  for (const Difference& diff : d.differences()) {
    if (diff.key == key) return true;
  }
  return false;
}

static const Difference* find_key(const RecordingDiff& d,
                                  const std::string& key) {
  for (const Difference& diff : d.differences()) {
    if (diff.key == key) return &diff;
  }
  return nullptr;
}

static const char* kBase =
    "device 1\n"
    "session 1\n"
    "ts 100\n"
    "tick 10\n"
    "session-begin class=1 fw=1.0.0 label=a\n"
    "schema id=1 name=m fields=v:u16\n"
    "state from=0 to=1 cause=1\n"
    "samples channel=1 delta=0\n"
    "  row 10\n"
    "  row 20\n"
    "end\n"
    "fault code=1 severity=warning\n"
    "session-end reason=0\n";

static void test_identical() {
  Decoded a, b;
  if (!load(kBase, a) || !load(kBase, b)) return;
  RecordingDiff d = RecordingDiff::compare(a.rec, b.rec);
  CHECK(d.identical());
  CHECK(d.summary().total() == 0);
  CHECK(render_diff(d) == "identical: no structural differences\n");
}

static void test_sample_count_change() {
  const char* more =
      "device 1\n"
      "session 1\n"
      "ts 100\n"
      "tick 10\n"
      "session-begin class=1 fw=1.0.0 label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "state from=0 to=1 cause=1\n"
      "samples channel=1 delta=0\n"
      "  row 10\n"
      "  row 20\n"
      "  row 30\n"
      "end\n"
      "fault code=1 severity=warning\n"
      "session-end reason=0\n";
  Decoded a, b;
  if (!load(kBase, a) || !load(more, b)) return;
  RecordingDiff d = RecordingDiff::compare(a.rec, b.rec);
  CHECK(!d.identical());
  const Difference* diff = find_key(d, "channel 1");
  CHECK(diff != nullptr);
  if (diff) {
    CHECK(diff->side == DiffSide::Changed);
    CHECK(diff->category == DiffCategory::Channel);
    CHECK(diff->detail == "samples 2 -> 3");
  }
}

static void test_extra_fault() {
  const char* more_faults =
      "device 1\n"
      "session 1\n"
      "ts 100\n"
      "tick 10\n"
      "session-begin class=1 fw=1.0.0 label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "state from=0 to=1 cause=1\n"
      "samples channel=1 delta=0\n"
      "  row 10\n"
      "  row 20\n"
      "end\n"
      "fault code=1 severity=warning\n"
      "fault code=2 severity=error\n"
      "fault code=3 severity=error\n"
      "session-end reason=0\n";
  Decoded a, b;
  if (!load(kBase, a) || !load(more_faults, b)) return;
  RecordingDiff d = RecordingDiff::compare(a.rec, b.rec);
  const Difference* err = find_key(d, "fault error");
  CHECK(err != nullptr);
  if (err) {
    CHECK(err->category == DiffCategory::FaultCount);
    CHECK(err->detail == "0 -> 2");
  }
  CHECK(!d.in_category(DiffCategory::FaultCount).empty());
}

static void test_session_added_removed() {
  const char* two_sessions =
      "device 1\n"
      "session 1\n"
      "ts 100\n"
      "tick 10\n"
      "session-begin class=1 fw=1.0.0 label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "state from=0 to=1 cause=1\n"
      "samples channel=1 delta=0\n"
      "  row 10\n"
      "  row 20\n"
      "end\n"
      "fault code=1 severity=warning\n"
      "session-end reason=0\n"
      "session-begin class=1 fw=1.0.0 label=b\n"
      "session-end reason=0\n";
  Decoded a, b;
  if (!load(kBase, a) || !load(two_sessions, b)) return;

  RecordingDiff d = RecordingDiff::compare(a.rec, b.rec);
  CHECK(has_key(d, "device 00000001 session #1"));
  const Difference* diff = find_key(d, "device 00000001 session #1");
  if (diff) CHECK(diff->side == DiffSide::RightOnly);
  CHECK(d.summary().right_only >= 1);

  RecordingDiff back = RecordingDiff::compare(b.rec, a.rec);
  const Difference* rev = find_key(back, "device 00000001 session #1");
  if (rev) CHECK(rev->side == DiffSide::LeftOnly);
  CHECK(back.summary().left_only >= 1);
}

static void test_channel_only_one_side() {
  const char* two_channels =
      "device 1\n"
      "session 1\n"
      "ts 100\n"
      "tick 10\n"
      "session-begin class=1 fw=1.0.0 label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "schema id=2 name=n fields=w:u8\n"
      "state from=0 to=1 cause=1\n"
      "samples channel=1 delta=0\n"
      "  row 10\n"
      "  row 20\n"
      "end\n"
      "samples channel=2 delta=0\n"
      "  row 3\n"
      "end\n"
      "fault code=1 severity=warning\n"
      "session-end reason=0\n";
  Decoded a, b;
  if (!load(kBase, a) || !load(two_channels, b)) return;
  RecordingDiff d = RecordingDiff::compare(a.rec, b.rec);
  const Difference* diff = find_key(d, "channel 2");
  CHECK(diff != nullptr);
  if (diff) {
    CHECK(diff->side == DiffSide::RightOnly);
    CHECK(diff->category == DiffCategory::Channel);
  }
}

static void test_schema_layout_change() {
  const char* wider =
      "device 1\n"
      "session 1\n"
      "ts 100\n"
      "tick 10\n"
      "session-begin class=1 fw=1.0.0 label=a\n"
      "schema id=1 name=m fields=v:u16,extra:u8\n"
      "state from=0 to=1 cause=1\n"
      "samples channel=1 delta=0\n"
      "  row 10 1\n"
      "  row 20 2\n"
      "end\n"
      "fault code=1 severity=warning\n"
      "session-end reason=0\n";
  Decoded a, b;
  if (!load(kBase, a) || !load(wider, b)) return;
  RecordingDiff d = RecordingDiff::compare(a.rec, b.rec);
  CHECK(!d.in_category(DiffCategory::SchemaVersion).empty());
}

static void test_session_fingerprint_change() {
  const char* diff_reason =
      "device 1\n"
      "session 1\n"
      "ts 100\n"
      "tick 10\n"
      "session-begin class=1 fw=1.0.0 label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "state from=0 to=1 cause=1\n"
      "samples channel=1 delta=0\n"
      "  row 10\n"
      "  row 20\n"
      "end\n"
      "fault code=1 severity=warning\n"
      "session-end reason=7\n";
  Decoded a, b;
  if (!load(kBase, a) || !load(diff_reason, b)) return;
  RecordingDiff d = RecordingDiff::compare(a.rec, b.rec);
  const Difference* diff = find_key(d, "device 00000001 session #0");
  CHECK(diff != nullptr);
  if (diff) {
    CHECK(diff->side == DiffSide::Changed);
    CHECK(diff->category == DiffCategory::Session);
  }
}

static void test_render_deterministic() {
  Decoded a, b;
  if (!load(kBase, a)) return;
  const char* other =
      "device 1\n"
      "session 1\n"
      "session-begin label=z\n"
      "session-end reason=0\n";
  if (!load(other, b)) return;
  RecordingDiff d = RecordingDiff::compare(a.rec, b.rec);
  CHECK(render_diff(d) == render_diff(d));
  CHECK(!d.identical());
}

int main() {
  test_identical();
  test_sample_count_change();
  test_extra_fault();
  test_session_added_removed();
  test_channel_only_one_side();
  test_schema_layout_change();
  test_session_fingerprint_change();
  test_render_deterministic();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_diff: all checks passed\n");
  return 0;
}
