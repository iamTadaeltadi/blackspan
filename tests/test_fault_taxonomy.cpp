#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/assemble.h"
#include "blackspan/fault_taxonomy.h"
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

static FaultEvent fe(uint16_t code, Severity sev, uint8_t subsys, uint32_t dev,
                     uint32_t seq, uint64_t ts) {
  FaultEvent f;
  f.fault_code = code;
  f.severity = sev;
  f.subsystem_id = subsys;
  f.device_id = dev;
  f.session_seq = seq;
  f.monotonic_ts = ts;
  return f;
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

static void test_classify_groups() {
  std::vector<FaultEvent> faults = {
      fe(0x10, Severity::Info, 1, 1, 0, 100),
      fe(0x10, Severity::Error, 1, 1, 1, 300),
      fe(0x10, Severity::Warning, 1, 2, 0, 200),
      fe(0x20, Severity::Critical, 2, 1, 2, 150),
  };

  std::vector<FaultClass> classes = classify_faults(faults);
  CHECK(classes.size() == 2);
  if (classes.size() == 2) {
    const FaultClass& a = classes[0];
    CHECK(a.fault_code == 0x10);
    CHECK(a.subsystem_id == 1);
    CHECK(a.count == 3);
    CHECK(a.worst_severity == Severity::Error);
    CHECK(a.first_ts == 100);
    CHECK(a.last_ts == 300);
    CHECK(a.device_count == 2);

    const FaultClass& b = classes[1];
    CHECK(b.fault_code == 0x20);
    CHECK(b.subsystem_id == 2);
    CHECK(b.count == 1);
    CHECK(b.worst_severity == Severity::Critical);
    CHECK(b.first_ts == 150);
    CHECK(b.last_ts == 150);
    CHECK(b.device_count == 1);

    CHECK(describe_fault_class(a).find("0010") != std::string::npos);
    CHECK(!render_taxonomy(classes).empty());
  }
}

static void test_subsystem_load_histogram() {
  std::vector<FaultEvent> faults = {
      fe(1, Severity::Info, 1, 1, 0, 10),
      fe(2, Severity::Warning, 1, 1, 1, 20),
      fe(3, Severity::Error, 1, 1, 2, 30),
      fe(4, Severity::Critical, 2, 1, 3, 40),
      fe(5, Severity::Critical, 2, 1, 4, 50),
  };

  std::vector<SubsystemLoad> loads = subsystem_load(faults);
  CHECK(loads.size() == 2);
  if (loads.size() == 2) {
    CHECK(loads[0].subsystem_id == 1);
    CHECK(loads[1].subsystem_id == 2);

    const SubsystemLoad& one = loads[0];
    CHECK(one.total == 3);
    CHECK(one.by_severity[0] == 1);
    CHECK(one.by_severity[1] == 1);
    CHECK(one.by_severity[2] == 1);
    CHECK(one.by_severity[3] == 0);

    const SubsystemLoad& two = loads[1];
    CHECK(two.total == 2);
    CHECK(two.by_severity[3] == 2);

    for (const SubsystemLoad& l : loads) {
      size_t sum = 0;
      for (size_t k = 0; k < 4; ++k) sum += l.by_severity[k];
      CHECK(sum == l.total);
    }
    CHECK(!describe_subsystem_load(one).empty());
  }
}

static void test_recurring_intervals() {
  std::vector<FaultEvent> faults = {
      fe(0xAA, Severity::Warning, 1, 1, 0, 400),
      fe(0xAA, Severity::Warning, 1, 1, 1, 100),
      fe(0xAA, Severity::Warning, 1, 1, 2, 200),
      fe(0xBB, Severity::Error, 1, 1, 3, 500),
      fe(0xAA, Severity::Info, 1, 2, 0, 1100),
      fe(0xAA, Severity::Info, 1, 2, 1, 1000),
  };

  std::vector<Recurrence> rec = recurring_faults(faults, 2);
  CHECK(rec.size() == 2);
  if (rec.size() == 2) {
    const Recurrence& first = rec[0];
    CHECK(first.fault_code == 0xAA);
    CHECK(first.device_id == 1);
    CHECK(first.occurrences == 3);
    CHECK(first.mean_interval_ns == 150);
    CHECK(first.min_interval_ns == 100);

    const Recurrence& second = rec[1];
    CHECK(second.fault_code == 0xAA);
    CHECK(second.device_id == 2);
    CHECK(second.occurrences == 2);
    CHECK(second.mean_interval_ns == 100);
    CHECK(second.min_interval_ns == 100);

    CHECK(describe_recurrence(first).find("00aa") != std::string::npos);
  }

  for (const Recurrence& r : rec) CHECK(r.fault_code != 0xBB);

  std::vector<Recurrence> loose = recurring_faults(faults, 1);
  bool found_single = false;
  for (const Recurrence& r : loose) {
    if (r.fault_code == 0xBB && r.device_id == 1) {
      found_single = true;
      CHECK(r.occurrences == 1);
      CHECK(r.mean_interval_ns == 0);
      CHECK(r.min_interval_ns == 0);
    }
  }
  CHECK(found_single);
}

static void test_severity_escalations() {
  std::vector<FaultEvent> faults = {
      fe(1, Severity::Info, 0, 1, 0, 100),
      fe(1, Severity::Warning, 0, 1, 1, 200),
      fe(1, Severity::Error, 0, 1, 2, 300),
      fe(1, Severity::Info, 0, 1, 3, 400),

      fe(1, Severity::Warning, 0, 2, 0, 100),
      fe(1, Severity::Warning, 0, 2, 1, 200),
      fe(1, Severity::Warning, 0, 2, 2, 300),

      fe(1, Severity::Error, 0, 3, 0, 100),
      fe(1, Severity::Warning, 0, 3, 1, 200),
      fe(1, Severity::Info, 0, 3, 2, 300),

      fe(1, Severity::Info, 0, 4, 0, 10),
      fe(1, Severity::Error, 0, 4, 1, 20),
  };

  std::vector<EscalationRun> runs = severity_escalations(faults, 3);
  CHECK(runs.size() == 1);
  if (runs.size() == 1) {
    const EscalationRun& r = runs[0];
    CHECK(r.device_id == 1);
    CHECK(r.start_ts == 100);
    CHECK(r.end_ts == 300);
    CHECK(r.length == 3);
    CHECK(r.peak == Severity::Error);
    CHECK(describe_escalation(r).find("peak") != std::string::npos);
  }

  std::vector<EscalationRun> shorter = severity_escalations(faults, 2);
  CHECK(shorter.size() == 2);
  if (shorter.size() == 2) {
    CHECK(shorter[0].device_id == 1);
    CHECK(shorter[0].length == 3);
    CHECK(shorter[1].device_id == 4);
    CHECK(shorter[1].length == 2);
    CHECK(shorter[1].peak == Severity::Error);
  }
}

static void test_summarize() {
  std::vector<FaultEvent> faults = {
      fe(0x10, Severity::Info, 1, 1, 0, 100),
      fe(0x10, Severity::Error, 1, 1, 1, 200),
      fe(0x10, Severity::Warning, 1, 2, 0, 300),
      fe(0x20, Severity::Critical, 2, 1, 2, 150),
  };

  TaxonomySummary s = summarize_taxonomy(faults);
  CHECK(s.total_faults == 4);
  CHECK(s.distinct_codes == 2);
  CHECK(s.distinct_subsystems == 2);
  CHECK(s.worst == Severity::Critical);
  CHECK(s.top_code == 0x10);
  CHECK(s.top_code_count == 3);
  CHECK(describe_taxonomy(s).find("0010") != std::string::npos);
}

static void test_summarize_tiebreak() {
  std::vector<FaultEvent> faults = {
      fe(5, Severity::Info, 1, 1, 0, 10),
      fe(5, Severity::Info, 1, 1, 1, 20),
      fe(3, Severity::Info, 1, 1, 2, 30),
      fe(3, Severity::Info, 1, 1, 3, 40),
  };

  TaxonomySummary s = summarize_taxonomy(faults);
  CHECK(s.top_code == 3);
  CHECK(s.top_code_count == 2);
}

static void test_empty_inputs() {
  std::vector<FaultEvent> none;
  CHECK(classify_faults(none).empty());
  CHECK(subsystem_load(none).empty());
  CHECK(recurring_faults(none, 2).empty());
  CHECK(severity_escalations(none, 2).empty());

  TaxonomySummary s = summarize_taxonomy(none);
  CHECK(s.total_faults == 0);
  CHECK(s.distinct_codes == 0);
  CHECK(s.distinct_subsystems == 0);
  CHECK(s.worst == Severity::Info);
  CHECK(s.top_code == 0);
  CHECK(s.top_code_count == 0);

  CHECK(!describe_taxonomy(s).empty());
  CHECK(!render_taxonomy(classify_faults(none)).empty());
}

static void test_taxonomy_of_recording() {
  const char* script =
      "device 1\n"
      "session 0\n"
      "fault code=0x10 severity=warning subsystem=1 ts=100\n"
      "fault code=0x10 severity=error subsystem=1 ts=200\n"
      "fault code=0x20 severity=info subsystem=2 ts=150\n"
      "device 2\n"
      "session 0\n"
      "fault code=0x10 severity=critical subsystem=1 ts=300\n";

  std::vector<uint8_t> bytes;
  DecodedRecording rec;
  if (!decode_script(script, bytes, rec)) return;

  CHECK(rec.anomalies().faults().size() == 4);

  std::vector<FaultClass> classes = taxonomy_of(rec);
  CHECK(classes.size() == 2);
  if (classes.size() == 2) {
    const FaultClass& top = classes[0];
    CHECK(top.fault_code == 0x10);
    CHECK(top.subsystem_id == 1);
    CHECK(top.count == 3);
    CHECK(top.worst_severity == Severity::Critical);
    CHECK(top.first_ts == 100);
    CHECK(top.last_ts == 300);
    CHECK(top.device_count == 2);

    const FaultClass& other = classes[1];
    CHECK(other.fault_code == 0x20);
    CHECK(other.count == 1);
    CHECK(other.device_count == 1);
  }

  TaxonomySummary s = summarize_taxonomy(rec.anomalies().faults());
  CHECK(s.total_faults == 4);
  CHECK(s.distinct_codes == 2);
  CHECK(s.worst == Severity::Critical);
  CHECK(s.top_code == 0x10);
}

int main() {
  test_classify_groups();
  test_subsystem_load_histogram();
  test_recurring_intervals();
  test_severity_escalations();
  test_summarize();
  test_summarize_tiebreak();
  test_empty_inputs();
  test_taxonomy_of_recording();
  if (failures) {
    std::fprintf(stderr, "%d checks failed\n", failures);
    return 1;
  }
  std::printf("all fault_taxonomy tests passed\n");
  return 0;
}
