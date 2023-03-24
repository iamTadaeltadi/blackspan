#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/pipeline.h"
#include "blackspan/timeline.h"

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

static const char* kScript =
    "device 0x11\n"
    "session 1\n"
    "ts 1000\n"
    "tick 100\n"
    "session-begin class=1 fw=1.0.0 label=a\n"
    "schema id=1 name=m fields=v:u16\n"
    "state from=0 to=1 cause=1\n"
    "samples channel=1 delta=0\n"
    "  row 10\n"
    "  row 20\n"
    "end\n"
    "config param=2 old=0 new=5\n"
    "fault code=7 severity=error subsystem=1\n"
    "marker kind=1 text=note\n"
    "checkpoint id=3 bytes=100\n"
    "state from=1 to=0 cause=2\n"
    "session-end reason=0\n";

static void test_build_and_order() {
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(kScript, bytes, rec)) return;

  Timeline tl = Timeline::build(rec);

  CHECK(tl.count_of(TimelineKind::SessionBegin) == 1);
  CHECK(tl.count_of(TimelineKind::SessionEnd) == 1);
  CHECK(tl.count_of(TimelineKind::StateTransition) == 2);
  CHECK(tl.count_of(TimelineKind::ConfigChange) == 1);
  CHECK(tl.count_of(TimelineKind::Fault) == 1);
  CHECK(tl.count_of(TimelineKind::Marker) == 1);
  CHECK(tl.count_of(TimelineKind::Checkpoint) == 1);
  CHECK(tl.count_of(TimelineKind::SampleBlock) == 1);
  CHECK(tl.count_of(TimelineKind::Reconnect) == 0);

  const std::vector<TimelineEvent>& e = tl.events();
  for (size_t i = 1; i < e.size(); ++i) {
    CHECK(e[i - 1].monotonic_ts <= e[i].monotonic_ts);
  }

  auto span = tl.span();
  CHECK(span.has_value());
  if (span.has_value()) {
    CHECK(span->first == 1000);
    CHECK(span->second == 1900);
  }

  bool saw_fault = false;
  for (const TimelineEvent& ev : e) {
    if (ev.kind == TimelineKind::Fault) {
      saw_fault = true;
      CHECK(ev.severity == Severity::Error);
      CHECK(ev.detail.find("state s1") != std::string::npos);
    }
  }
  CHECK(saw_fault);

  bool saw_block = false;
  for (const TimelineEvent& ev : e) {
    if (ev.kind == TimelineKind::SampleBlock) {
      saw_block = true;
      CHECK(ev.channel_id == 1);
      CHECK(ev.count == 2);
    }
  }
  CHECK(saw_block);
}

static void test_kind_filter() {
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(kScript, bytes, rec)) return;

  TimelineOptions opts;
  opts.kind_mask = timeline_kind_bit(TimelineKind::Fault) |
                   timeline_kind_bit(TimelineKind::StateTransition);
  Timeline tl = Timeline::build(rec, opts);
  CHECK(tl.size() == 3);
  for (const TimelineEvent& e : tl.events()) {
    CHECK(e.kind == TimelineKind::Fault ||
          e.kind == TimelineKind::StateTransition);
  }
}

static void test_time_filter() {
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(kScript, bytes, rec)) return;

  TimelineOptions opts;
  opts.since = 1400;
  opts.until = 1700;
  Timeline tl = Timeline::build(rec, opts);
  for (const TimelineEvent& e : tl.events()) {
    CHECK(e.monotonic_ts >= 1400 && e.monotonic_ts <= 1700);
  }

  CHECK(tl.size() == 4);
}

static void test_device_filter_multi() {
  const char* script =
      "tick 10\n"
      "device 1\n"
      "session 1\n"
      "ts 100\n"
      "session-begin label=a\n"
      "fault code=1 severity=warning\n"
      "device 2\n"
      "session 1\n"
      "ts 200\n"
      "session-begin label=b\n"
      "fault code=2 severity=error\n";
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(script, bytes, rec)) return;

  Timeline all = Timeline::build(rec);
  CHECK(all.devices().size() == 2);

  TimelineOptions opts;
  opts.device = 2;
  Timeline just2 = Timeline::build(rec, opts);
  CHECK(!just2.empty());
  for (const TimelineEvent& e : just2.events()) {
    CHECK(e.device_id == 2);
  }
  CHECK(just2.count_of(TimelineKind::Fault) == 1);
}

static void test_window_and_density() {
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(kScript, bytes, rec)) return;

  Timeline tl = Timeline::build(rec);
  std::vector<const TimelineEvent*> win = tl.in_window(1500, 1600);
  for (const TimelineEvent* e : win) {
    CHECK(e->monotonic_ts >= 1500 && e->monotonic_ts <= 1600);
  }
  CHECK(!win.empty());

  std::vector<size_t> d = tl.density(300);
  CHECK(d.size() == 4);
  size_t total = 0;
  for (size_t c : d) total += c;
  CHECK(total == tl.size());

  CHECK(tl.density(0).empty());
  Timeline empty;
  CHECK(empty.density(100).empty());
  CHECK(empty.span() == std::nullopt);
}

static void test_render() {
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(kScript, bytes, rec)) return;

  Timeline tl = Timeline::build(rec);
  std::string a = render_timeline(tl);
  std::string b = render_timeline(tl);
  CHECK(a == b);
  CHECK(a.find("events over ts 1000..1900") != std::string::npos);
  CHECK(a.find("fault") != std::string::npos);

  Timeline empty;
  CHECK(render_timeline(empty) == "0 events\n");
}

int main() {
  test_build_and_order();
  test_kind_filter();
  test_time_filter();
  test_device_filter_multi();
  test_window_and_density();
  test_render();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_timeline: all checks passed\n");
  return 0;
}
