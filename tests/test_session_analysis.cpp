#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/pipeline.h"
#include "blackspan/session_analysis.h"

using namespace blackspan;

static int failures = 0;
#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__, \
                   __LINE__);                                             \
      ++failures;                                                         \
    }                                                                     \
  } while (0)

static bool near(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps;
}

static bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// Assembles a script, asserting it produced no diagnostics, and returns the bytes.
static std::vector<uint8_t> assemble(const std::string& script) {
  AssembleResult a = assemble_script(script);
  CHECK(a.ok());
  if (!a.ok()) {
    for (const auto& e : a.errors) {
      std::fprintf(stderr, "  assemble error: %s\n",
                   describe_assemble_diagnostic(e).c_str());
    }
  }
  return a.bytes;
}

// Locates the activity rollup for a device in an analyze_activity() result.
static const DeviceActivity* find_dev(const std::vector<DeviceActivity>& v,
                                      uint32_t id) {
  for (const auto& a : v)
    if (a.device_id == id) return &a;
  return nullptr;
}

// ---- tests ----------------------------------------------------------------

// Two sessions of one device separated by a known idle gap: active is the sum of
// the two durations, idle is the single gap, and duty is their ratio.
static void test_two_sessions_one_gap() {
  const std::vector<uint8_t> wire = assemble(
      "device 1\n"
      "session-begin seq=0 ts=1000 label=s0\n"
      "session-end   seq=1 ts=2000 reason=0\n"
      "session-begin seq=2 ts=3000 label=s1\n"
      "session-end   seq=3 ts=3500 reason=0\n");

  DecodedRecording rec;
  rec.decode(wire.data(), wire.size());
  const std::vector<Session>& sessions = rec.sessions().sessions();

  // Cross-check the reconstruction the analysis is built on.
  CHECK(sessions.size() == 2);
  CHECK(sessions[0].begin_ts == 1000 && sessions[0].end_ts == 2000);
  CHECK(sessions[0].closed && sessions[0].duration_ns() == 1000);
  CHECK(sessions[1].begin_ts == 3000 && sessions[1].end_ts == 3500);
  CHECK(sessions[1].closed && sessions[1].duration_ns() == 500);

  const std::vector<DeviceActivity> act = analyze_activity(sessions);
  CHECK(act.size() == 1);
  const DeviceActivity& a = act[0];
  CHECK(a.device_id == 1);
  CHECK(a.session_count == 2);
  CHECK(a.first_begin_ts == 1000);
  CHECK(a.last_end_ts == 3500);
  CHECK(a.total_active_ns == 1500);
  CHECK(a.total_gap_ns == 1000);
  CHECK(near(a.duty_cycle, 1500.0 / 2500.0));
  CHECK(a.unclosed_sessions == 0);
  CHECK(a.overlaps == 0);

  const std::vector<SessionGap> gaps = session_gaps(sessions);
  CHECK(gaps.size() == 1);
  CHECK(gaps[0].device_id == 1);
  CHECK(gaps[0].after_index == 0);
  CHECK(gaps[0].before_index == 1);
  CHECK(gaps[0].gap_ns == 1000);

  CHECK(near(device_duty_cycle(sessions, 1), 1500.0 / 2500.0));
  CHECK(near(device_duty_cycle(sessions, 999), 0.0));  // unknown device

  const std::string d = describe_activity(a);
  CHECK(has(d, "device 00000001"));
  CHECK(has(d, "2 sessions"));
  CHECK(has(d, "duty 0.60"));
}

// Several devices, authored out of device-id order and interleaved, are rolled up
// independently and returned sorted by device_id ascending.
static void test_multiple_devices_sorted() {
  const std::vector<uint8_t> wire = assemble(
      "device 3\n"
      "session-begin seq=0 ts=100 label=d3\n"
      "session-end   seq=1 ts=200 reason=0\n"
      "device 1\n"
      "session-begin seq=0 ts=50 label=d1\n"
      "session-end   seq=1 ts=150 reason=0\n"
      "device 2\n"
      "session-begin seq=0 ts=500 label=d2\n"
      "session-end   seq=1 ts=900 reason=0\n");

  DecodedRecording rec;
  rec.decode(wire.data(), wire.size());
  const std::vector<Session>& sessions = rec.sessions().sessions();
  CHECK(sessions.size() == 3);

  const std::vector<DeviceActivity> act = analyze_activity(sessions);
  CHECK(act.size() == 3);
  CHECK(act[0].device_id == 1);
  CHECK(act[1].device_id == 2);
  CHECK(act[2].device_id == 3);

  // Each device saw exactly one session, so each is fully active (duty 1.0) and
  // contributes no gaps.
  for (const DeviceActivity& a : act) {
    CHECK(a.session_count == 1);
    CHECK(a.total_gap_ns == 0);
    CHECK(near(a.duty_cycle, 1.0));
  }
  CHECK(find_dev(act, 1)->total_active_ns == 100);
  CHECK(find_dev(act, 2)->total_active_ns == 400);
  CHECK(find_dev(act, 3)->total_active_ns == 100);

  CHECK(session_gaps(sessions).empty());
}

// An unterminated trailing session is counted as unclosed and contributes no
// active time, while checkpoints and reconnects across both sessions roll up.
static void test_unclosed_and_rollups() {
  const std::vector<uint8_t> wire = assemble(
      "device 7\n"
      "session-begin seq=0 ts=1000 label=a\n"
      "checkpoint    seq=1 ts=1100 id=1 bytes=10\n"
      "reconnect     seq=2 ts=1200 gap=5 prev=0\n"
      "session-end   seq=3 ts=2000 reason=0\n"
      "session-begin seq=4 ts=3000 label=b\n"
      "checkpoint    seq=5 ts=3100 id=2 bytes=20\n");  // never closed

  DecodedRecording rec;
  rec.decode(wire.data(), wire.size());
  const std::vector<Session>& sessions = rec.sessions().sessions();
  CHECK(sessions.size() == 2);
  CHECK(sessions[0].closed);
  CHECK(!sessions[1].closed);
  CHECK(sessions[1].end_ts == 3100);      // last observed record
  CHECK(sessions[1].duration_ns() == 0);  // unclosed -> no duration

  const std::vector<DeviceActivity> act = analyze_activity(sessions);
  CHECK(act.size() == 1);
  const DeviceActivity& a = act[0];
  CHECK(a.session_count == 2);
  CHECK(a.unclosed_sessions == 1);
  CHECK(a.total_checkpoints == 2);
  CHECK(a.total_reconnects == 1);
  CHECK(a.total_active_ns == 1000);  // only the closed session
  CHECK(a.total_gap_ns == 1000);     // 3000 - 2000
  CHECK(near(a.duty_cycle, 1000.0 / 2000.0));

  const std::string d = describe_activity(a);
  CHECK(has(d, "1 unclosed"));
  CHECK(has(d, "2 checkpoints"));
  CHECK(has(d, "1 reconnects"));
}

// A single session: no gaps, duty 1.0 for a positive duration and 0.0 for a
// zero-duration session.
static void test_single_session() {
  const std::vector<uint8_t> wire = assemble(
      "device 5\n"
      "session-begin seq=0 ts=1000 label=pos\n"
      "session-end   seq=1 ts=1500 reason=0\n"
      "device 6\n"
      "session-begin seq=0 ts=2000 label=zero\n"
      "session-end   seq=1 ts=2000 reason=0\n");

  DecodedRecording rec;
  rec.decode(wire.data(), wire.size());
  const std::vector<Session>& sessions = rec.sessions().sessions();
  CHECK(sessions.size() == 2);

  const std::vector<DeviceActivity> act = analyze_activity(sessions);
  CHECK(act.size() == 2);

  const DeviceActivity* pos = find_dev(act, 5);
  const DeviceActivity* zero = find_dev(act, 6);
  CHECK(pos && zero);
  CHECK(pos->session_count == 1 && pos->total_gap_ns == 0);
  CHECK(pos->total_active_ns == 500);
  CHECK(near(pos->duty_cycle, 1.0));

  CHECK(zero->session_count == 1 && zero->total_gap_ns == 0);
  CHECK(zero->total_active_ns == 0);
  CHECK(zero->first_begin_ts == 2000 && zero->last_end_ts == 2000);
  CHECK(near(zero->duty_cycle, 0.0));  // zero span -> 0, not 1

  CHECK(session_gaps(sessions).empty());
  CHECK(near(device_duty_cycle(sessions, 5), 1.0));
  CHECK(near(device_duty_cycle(sessions, 6), 0.0));
}

// Overlapping sessions (the later one begins before the earlier one ends): the
// gap is clamped to 0 and the overlap is counted.
static void test_overlapping_sessions() {
  const std::vector<uint8_t> wire = assemble(
      "device 9\n"
      "session-begin seq=0 ts=1000 label=o0\n"
      "session-end   seq=1 ts=3000 reason=0\n"
      "session-begin seq=2 ts=2000 label=o1\n"  // begins before o0 ended (3000)
      "session-end   seq=3 ts=4000 reason=0\n");

  DecodedRecording rec;
  rec.decode(wire.data(), wire.size());
  const std::vector<Session>& sessions = rec.sessions().sessions();
  CHECK(sessions.size() == 2);

  const std::vector<DeviceActivity> act = analyze_activity(sessions);
  CHECK(act.size() == 1);
  const DeviceActivity& a = act[0];
  CHECK(a.session_count == 2);
  CHECK(a.total_active_ns == 4000);  // 2000 + 2000
  CHECK(a.total_gap_ns == 0);        // overlap clamps the only gap to 0
  CHECK(a.overlaps == 1);
  CHECK(near(a.duty_cycle, 1.0));

  const std::vector<SessionGap> gaps = session_gaps(sessions);
  CHECK(gaps.size() == 1);
  CHECK(gaps[0].after_index == 0);
  CHECK(gaps[0].before_index == 1);
  CHECK(gaps[0].gap_ns == 0);

  CHECK(has(describe_activity(a), "1 overlaps"));
}

// The fleet rollup folds per-device activity into one recording-level summary,
// and distinguishes nanosecond-weighted from device-weighted duty cycle.
static void test_fleet_rollup() {
  // Device 1: one 1000ns session, fully active (duty 1.0).
  // Device 2: two 500ns sessions with a 1000ns gap between them (duty 0.5).
  const std::vector<uint8_t> wire = assemble(
      "device 1\n"
      "session-begin seq=0 ts=1000 label=a\n"
      "session-end   seq=1 ts=2000 reason=0\n"
      "device 2\n"
      "session-begin seq=0 ts=100 label=b0\n"
      "session-end   seq=1 ts=600 reason=0\n"
      "session-begin seq=2 ts=1600 label=b1\n"
      "session-end   seq=3 ts=2100 reason=0\n");

  DecodedRecording rec;
  rec.decode(wire.data(), wire.size());
  const std::vector<Session>& sessions = rec.sessions().sessions();

  const std::vector<DeviceActivity> act = analyze_activity(sessions);
  CHECK(act.size() == 2);

  const FleetActivity f = analyze_fleet(act);
  CHECK(f.has_devices);
  CHECK(f.device_count == 2);
  CHECK(f.session_count == 3);
  CHECK(f.earliest_begin_ts == 100);   // device 2's first session
  CHECK(f.latest_end_ts == 2100);      // device 2's last session
  CHECK(f.total_active_ns == 2000);    // 1000 + (500 + 500)
  CHECK(f.total_gap_ns == 1000);       // device 2's single gap
  CHECK(near(f.aggregate_duty_cycle, 2000.0 / 3000.0));  // ns-weighted
  CHECK(near(f.mean_duty_cycle, (1.0 + 0.5) / 2.0));     // device-weighted
  // Both devices total 1000ns active, so it is a tie; the lower id (1) wins.
  CHECK(f.busiest_device == 1);
  CHECK(f.busiest_active_ns == 1000);
  CHECK(f.unclosed_sessions == 0);
  CHECK(f.total_overlaps == 0);

  const std::string d = describe_fleet(f);
  CHECK(has(d, "2 devices"));
  CHECK(has(d, "3 sessions"));
  CHECK(has(d, "busiest device 00000001"));

  // Empty fleet describes cleanly.
  const FleetActivity empty = analyze_fleet({});
  CHECK(!empty.has_devices);
  CHECK(describe_fleet(empty) == "fleet: 0 devices");
}

// longest_idle_gap surfaces the single darkest stretch across all devices.
static void test_longest_idle_gap() {
  // Device 5 has a 2000ns gap; device 8 has a 500ns gap. The former wins.
  const std::vector<uint8_t> wire = assemble(
      "device 5\n"
      "session-begin seq=0 ts=1000 label=a\n"
      "session-end   seq=1 ts=2000 reason=0\n"
      "session-begin seq=2 ts=4000 label=b\n"  // gap 2000
      "session-end   seq=3 ts=4500 reason=0\n"
      "device 8\n"
      "session-begin seq=0 ts=100 label=c\n"
      "session-end   seq=1 ts=200 reason=0\n"
      "session-begin seq=2 ts=700 label=d\n"  // gap 500
      "session-end   seq=3 ts=900 reason=0\n");

  DecodedRecording rec;
  rec.decode(wire.data(), wire.size());
  const std::vector<Session>& sessions = rec.sessions().sessions();

  CHECK(session_gaps(sessions).size() == 2);

  SessionGap g;
  CHECK(longest_idle_gap(sessions, g));
  CHECK(g.device_id == 5);
  CHECK(g.after_index == 0);
  CHECK(g.before_index == 1);
  CHECK(g.gap_ns == 2000);

  // A recording with no inter-session gaps reports none.
  const std::vector<uint8_t> single = assemble(
      "device 1\n"
      "session-begin seq=0 ts=1000 label=only\n"
      "session-end   seq=1 ts=1500 reason=0\n");
  DecodedRecording rec2;
  rec2.decode(single.data(), single.size());
  SessionGap none_gap;
  CHECK(!longest_idle_gap(rec2.sessions().sessions(), none_gap));

  // Empty input reports no gap.
  const std::vector<Session> empty;
  SessionGap unused;
  CHECK(!longest_idle_gap(empty, unused));
}

// Empty input yields empty results; describe_activity is still non-empty.
static void test_empty_and_describe() {
  const std::vector<Session> none;
  CHECK(analyze_activity(none).empty());
  CHECK(session_gaps(none).empty());
  CHECK(near(device_duty_cycle(none, 0), 0.0));

  DeviceActivity def;
  const std::string d = describe_activity(def);
  CHECK(!d.empty());
  CHECK(has(d, "device 00000000"));
  CHECK(has(d, "0 sessions"));
}

int main() {
  test_two_sessions_one_gap();
  test_multiple_devices_sorted();
  test_unclosed_and_rollups();
  test_single_session();
  test_overlapping_sessions();
  test_fleet_rollup();
  test_longest_idle_gap();
  test_empty_and_describe();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all session_analysis tests passed\n");
  return 0;
}
