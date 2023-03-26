#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/assemble.h"
#include "blackspan/correlate.h"
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

static CorrelatedEvent ev(EventClass cls, uint32_t device, uint64_t ts,
                          uint32_t code) {
  CorrelatedEvent e;
  e.cls = cls;
  e.device_id = device;
  e.ts = ts;
  e.code = code;
  return e;
}

static CorrelationConfig cfg_of(uint64_t window, size_t min_members) {
  CorrelationConfig c;
  c.window_ns = window;
  c.min_members = min_members;
  return c;
}

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

static void test_class_names() {
  CHECK(std::string(event_class_name(EventClass::Detection)) == "detection");
  CHECK(std::string(event_class_name(EventClass::Fault)) == "fault");
  CHECK(std::string(event_class_name(EventClass::Transition)) == "transition");
}

static void test_empty_input() {
  std::vector<CorrelatedEvent> none;
  CHECK(correlate_events(none, cfg_of(1000000, 2)).empty());
}

static void test_basic_pair_and_lone() {
  std::vector<CorrelatedEvent> events = {
      ev(EventClass::Detection, 1, 1000, 7),
      ev(EventClass::Fault, 1, 1500, 42),
  };
  std::vector<Correlation> cs = correlate_events(events, cfg_of(1000000, 2));
  CHECK(cs.size() == 1);
  if (cs.size() == 1) {
    CHECK(cs[0].device_id == 1);
    CHECK(cs[0].anchor_ts == 1000);
    CHECK(cs[0].detections == 1);
    CHECK(cs[0].faults == 1);
    CHECK(cs[0].transitions == 0);
    CHECK(cs[0].members.size() == 2);
    CHECK(cs[0].members[0].cls == EventClass::Detection);
    CHECK(cs[0].members[1].cls == EventClass::Fault);
    CHECK(!describe_correlation(cs[0]).empty());
  }

  std::vector<CorrelatedEvent> lone = {ev(EventClass::Detection, 1, 1000, 7)};
  CHECK(correlate_events(lone, cfg_of(1000000, 2)).empty());
}

static void test_outside_window_not_merged() {
  std::vector<CorrelatedEvent> events = {
      ev(EventClass::Detection, 1, 1000, 7),
      ev(EventClass::Fault, 1, 2000, 42),
  };
  CHECK(correlate_events(events, cfg_of(100, 2)).empty());
}

static void test_two_devices_independent() {
  std::vector<CorrelatedEvent> events = {
      ev(EventClass::Fault, 2, 5000, 9),
      ev(EventClass::Detection, 2, 5100, 3),
      ev(EventClass::Detection, 1, 1000, 7),
      ev(EventClass::Transition, 1, 1200, 0x00010002u),
  };
  std::vector<Correlation> cs = correlate_events(events, cfg_of(1000000, 2));
  CHECK(cs.size() == 2);
  if (cs.size() == 2) {
    CHECK(cs[0].device_id == 1);
    CHECK(cs[0].anchor_ts == 1000);
    CHECK(cs[0].detections == 1);
    CHECK(cs[0].transitions == 1);
    CHECK(cs[0].faults == 0);

    CHECK(cs[1].device_id == 2);
    CHECK(cs[1].anchor_ts == 5000);
    CHECK(cs[1].detections == 1);
    CHECK(cs[1].faults == 1);
    CHECK(cs[1].transitions == 0);
    CHECK(cs[1].members[0].cls == EventClass::Fault);
    CHECK(cs[1].members[1].cls == EventClass::Detection);
  }
}

static void test_min_members_and_distinct_rule() {
  std::vector<CorrelatedEvent> three_det = {
      ev(EventClass::Detection, 1, 1000, 1),
      ev(EventClass::Detection, 1, 1010, 2),
      ev(EventClass::Detection, 1, 1020, 3),
  };
  CHECK(correlate_events(three_det, cfg_of(1000000, 2)).empty());

  std::vector<CorrelatedEvent> pair = {
      ev(EventClass::Detection, 1, 1000, 1),
      ev(EventClass::Fault, 1, 1010, 2),
  };
  CHECK(correlate_events(pair, cfg_of(1000000, 3)).empty());
  CHECK(correlate_events(pair, cfg_of(1000000, 2)).size() == 1);

  std::vector<CorrelatedEvent> trio = {
      ev(EventClass::Detection, 1, 1000, 1),
      ev(EventClass::Detection, 1, 1005, 2),
      ev(EventClass::Fault, 1, 1010, 3),
  };
  std::vector<Correlation> cs = correlate_events(trio, cfg_of(1000000, 3));
  CHECK(cs.size() == 1);
  if (cs.size() == 1) {
    CHECK(cs[0].detections == 2);
    CHECK(cs[0].faults == 1);
    CHECK(cs[0].members.size() == 3);
  }
}

static void test_window_boundary_precise() {
  std::vector<CorrelatedEvent> inclusive = {
      ev(EventClass::Detection, 1, 1000, 1),
      ev(EventClass::Fault, 1, 1100, 2),
  };
  std::vector<Correlation> in = correlate_events(inclusive, cfg_of(100, 2));
  CHECK(in.size() == 1);
  if (in.size() == 1) {
    CHECK(in[0].members.size() == 2);
    CHECK(in[0].detections == 1 && in[0].faults == 1);
  }

  std::vector<CorrelatedEvent> exclusive = {
      ev(EventClass::Detection, 1, 1000, 1),
      ev(EventClass::Fault, 1, 1101, 2),
  };
  CHECK(correlate_events(exclusive, cfg_of(100, 2)).empty());
}

static void test_non_overlapping_windows() {
  std::vector<CorrelatedEvent> events = {
      ev(EventClass::Detection, 1, 1000, 1),
      ev(EventClass::Fault, 1, 1050, 2),
      ev(EventClass::Detection, 1, 5000, 3),
      ev(EventClass::Transition, 1, 5060, 0x00020003u),
  };
  std::vector<Correlation> cs = correlate_events(events, cfg_of(100, 2));
  CHECK(cs.size() == 2);
  if (cs.size() == 2) {
    CHECK(cs[0].anchor_ts == 1000);
    CHECK(cs[0].detections == 1 && cs[0].faults == 1);
    CHECK(cs[1].anchor_ts == 5000);
    CHECK(cs[1].detections == 1 && cs[1].transitions == 1);
  }
}

static void test_recording_single_device() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"
      "  row 10\n"
      "  row 200\n"
      "end\n"
      "fault code=42 severity=error subsystem=1 ts=1000\n"
      "session-end reason=0 ts=2000\n";
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(script, bytes, rec)) return;

  DetectRuleParse rp = parse_detect_rules("1 0 high 100\n");
  CHECK(rp.ok());

  std::vector<CorrelatedEvent> events = gather_events(rec, rp.rules);
  size_t det = 0;
  size_t flt = 0;
  for (const CorrelatedEvent& e : events) {
    if (e.cls == EventClass::Detection) {
      ++det;
      CHECK(e.device_id == 1);
      CHECK(e.ts == 1000);
    } else if (e.cls == EventClass::Fault) {
      ++flt;
      CHECK(e.device_id == 1);
      CHECK(e.code == 42);
    }
  }
  CHECK(det == 1);
  CHECK(flt == 1);

  std::vector<Correlation> cs =
      correlate_recording(rec, rp.rules, cfg_of(1000000, 2));
  CHECK(cs.size() == 1);
  if (cs.size() == 1) {
    CHECK(cs[0].device_id == 1);
    CHECK(cs[0].anchor_ts == 1000);
    CHECK(cs[0].detections == 1);
    CHECK(cs[0].faults == 1);
    CHECK(cs[0].transitions == 0);
    CHECK(!describe_correlation(cs[0]).empty());
    CHECK(describe_correlation(cs[0]).find("device") != std::string::npos);
  }
}

static void test_recording_two_devices() {
  const char* script =
      "device 1\n"
      "session 1\n"
      "ts 1000\n"
      "session-begin label=a\n"
      "schema id=1 name=m fields=v:u16\n"
      "samples channel=1 delta=0 ts=1000\n"
      "  row 10\n"
      "  row 200\n"
      "end\n"
      "fault code=42 severity=error subsystem=1 ts=1000\n"
      "session-end reason=0 ts=1500\n"
      "device 2\n"
      "session 1\n"
      "ts 5000\n"
      "session-begin label=b\n"
      "schema id=2 name=n fields=v:u16\n"
      "samples channel=2 delta=0 ts=5000\n"
      "  row 10\n"
      "  row 300\n"
      "end\n"
      "fault code=99 severity=error subsystem=2 ts=5000\n"
      "session-end reason=0 ts=5500\n";
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(script, bytes, rec)) return;

  DetectRuleParse rp = parse_detect_rules(
      "1 0 high 100\n"
      "2 0 high 100\n");
  CHECK(rp.ok());

  std::vector<Correlation> cs =
      correlate_recording(rec, rp.rules, cfg_of(1000000, 2));
  CHECK(cs.size() == 2);
  if (cs.size() == 2) {
    CHECK(cs[0].device_id == 1);
    CHECK(cs[0].anchor_ts == 1000);
    CHECK(cs[0].detections == 1);
    CHECK(cs[0].faults == 1);

    CHECK(cs[1].device_id == 2);
    CHECK(cs[1].anchor_ts == 5000);
    CHECK(cs[1].detections == 1);
    CHECK(cs[1].faults == 1);
  }
}

static void test_recording_empty() {
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
      "session-end reason=0 ts=2000\n";
  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(script, bytes, rec)) return;

  DetectRuleParse rp = parse_detect_rules("1 0 high 100\n");
  CHECK(rp.ok());

  std::vector<CorrelatedEvent> events = gather_events(rec, rp.rules);
  for (const CorrelatedEvent& e : events) {
    CHECK(e.cls != EventClass::Detection);
  }
  std::vector<Correlation> cs =
      correlate_recording(rec, rp.rules, cfg_of(1000000, 2));
  CHECK(cs.empty());
}

static void test_analysis_helpers() {
  std::vector<CorrelatedEvent> events = {
      ev(EventClass::Detection, 1, 1000, 1),
      ev(EventClass::Fault, 1, 1050, 2),
      ev(EventClass::Detection, 2, 5000, 3),
      ev(EventClass::Fault, 2, 5040, 4),
      ev(EventClass::Transition, 2, 5060, 0x00010002u),
  };
  std::vector<Correlation> cs = correlate_events(events, cfg_of(1000000, 2));
  CHECK(cs.size() == 2);
  if (cs.size() != 2) return;

  CHECK(correlation_span(cs[0]) == 50);
  CHECK(correlation_span(cs[1]) == 60);

  CHECK(correlation_contains(cs[0], EventClass::Detection));
  CHECK(correlation_contains(cs[0], EventClass::Fault));
  CHECK(!correlation_contains(cs[0], EventClass::Transition));
  CHECK(correlation_contains(cs[1], EventClass::Transition));

  CHECK(correlation_strength(cs[1]) > correlation_strength(cs[0]));

  std::vector<size_t> rank = rank_correlations(cs);
  CHECK(rank.size() == 2);
  if (rank.size() == 2) {
    CHECK(rank[0] == 1);
    CHECK(rank[1] == 0);
  }

  std::vector<Correlation> d1 = correlations_for_device(cs, 1);
  CHECK(d1.size() == 1);
  if (d1.size() == 1) CHECK(d1[0].device_id == 1);

  std::vector<Correlation> with_trans =
      correlations_with_class(cs, EventClass::Transition);
  CHECK(with_trans.size() == 1);
  if (with_trans.size() == 1) CHECK(with_trans[0].device_id == 2);
  CHECK(correlations_with_class(cs, EventClass::Fault).size() == 2);

  std::string rendered = render_correlations(cs);
  CHECK(!rendered.empty());
  CHECK(rendered.find("correlation(s)") != std::string::npos);
  CHECK(rendered.find("device") != std::string::npos);

  CorrelationSummary sum = summarize_correlations(cs);
  CHECK(sum.total == 2);
  CHECK(sum.device_count == 2);
  CHECK(sum.detections == 2);
  CHECK(sum.faults == 2);
  CHECK(sum.transitions == 1);
  CHECK(sum.largest_members == 3);
  CHECK(sum.widest_span == 60);
  CHECK(!describe_correlation_summary(sum).empty());

  CHECK(summarize_correlations(std::vector<Correlation>{}).total == 0);
}

static void test_coincidences() {
  std::vector<CorrelatedEvent> events = {
      ev(EventClass::Detection, 1, 1000, 10),
      ev(EventClass::Fault, 1, 1050, 42),
      ev(EventClass::Fault, 1, 900, 7),
      ev(EventClass::Detection, 1, 3000, 11),
      ev(EventClass::Detection, 2, 5000, 12),
  };
  std::vector<Coincidence> co = detection_fault_coincidences(events, 100);
  CHECK(co.size() == 1);
  if (co.size() == 1) {
    CHECK(co[0].device_id == 1);
    CHECK(co[0].detection_ts == 1000);
    CHECK(co[0].detection_code == 10);
    CHECK(co[0].fault_ts == 1050);
    CHECK(co[0].fault_code == 42);
    CHECK(co[0].gap == 50);
    CHECK(!describe_coincidence(co[0]).empty());
  }

  std::vector<CorrelatedEvent> predecessor = {
      ev(EventClass::Detection, 1, 1000, 5),
      ev(EventClass::Fault, 1, 990, 8),
      ev(EventClass::Fault, 1, 1300, 9),
  };
  std::vector<Coincidence> pc = detection_fault_coincidences(predecessor, 100);
  CHECK(pc.size() == 1);
  if (pc.size() == 1) {
    CHECK(pc[0].fault_ts == 990);
    CHECK(pc[0].fault_code == 8);
    CHECK(pc[0].gap == 10);
  }

  std::vector<CorrelatedEvent> past_all = {
      ev(EventClass::Detection, 1, 2000, 5),
      ev(EventClass::Fault, 1, 990, 8),
      ev(EventClass::Fault, 1, 1300, 9),
  };
  std::vector<Coincidence> pa = detection_fault_coincidences(past_all, 1000);
  CHECK(pa.size() == 1);
  if (pa.size() == 1) {
    CHECK(pa[0].fault_ts == 1300);
    CHECK(pa[0].gap == 700);
  }
  CHECK(detection_fault_coincidences(past_all, 100).empty());

  std::vector<CorrelatedEvent> none;
  CHECK(detection_fault_coincidences(none, 100).empty());
}

int main() {
  test_class_names();
  test_empty_input();
  test_basic_pair_and_lone();
  test_outside_window_not_merged();
  test_two_devices_independent();
  test_min_members_and_distinct_rule();
  test_window_boundary_precise();
  test_non_overlapping_windows();
  test_recording_single_device();
  test_recording_two_devices();
  test_recording_empty();
  test_analysis_helpers();
  test_coincidences();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_correlate: all checks passed\n");
  return 0;
}
