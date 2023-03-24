#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/assemble.h"
#include "blackspan/channel.h"
#include "blackspan/derive.h"
#include "blackspan/export.h"
#include "blackspan/frame.h"
#include "blackspan/pipeline.h"
#include "blackspan/query.h"
#include "blackspan/report.h"
#include "blackspan/schema.h"
#include "blackspan/session.h"
#include "blackspan/stats.h"

#ifndef BLACKSPAN_EXAMPLES_DIR
#define BLACKSPAN_EXAMPLES_DIR "examples"
#endif

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

static std::string example_path(const char* name, const char* ext) {
  return std::string(BLACKSPAN_EXAMPLES_DIR) + "/" + name + ext;
}

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.clear();
  if (n > 0) {
    out.resize(static_cast<size_t>(n));
    if (std::fread(out.data(), 1, out.size(), f) != out.size()) {
      std::fclose(f);
      return false;
    }
  }
  std::fclose(f);
  return true;
}

static bool load_example(const char* name, std::vector<uint8_t>& bytes) {
  std::vector<uint8_t> script_bytes;
  if (!read_file(example_path(name, ".bsr"), script_bytes)) return false;

  AssembleResult r =
      assemble_script(std::string(script_bytes.begin(), script_bytes.end()));
  for (const AssembleDiagnostic& d : r.errors) {
    std::fprintf(stderr, "%s.bsr: %s\n", name,
                 describe_assemble_diagnostic(d).c_str());
  }
  if (!r.ok()) {
    ++failures;
    return false;
  }

  std::vector<uint8_t> committed;
  if (!read_file(example_path(name, ".bsp"), committed)) return false;
  if (r.bytes != committed) {
    std::fprintf(stderr,
                 "%s.bsr no longer assembles to %s.bsp (%zu vs %zu bytes) - "
                 "regenerate with examples/check.sh --update\n",
                 name, name, r.bytes.size(), committed.size());
    ++failures;
    return false;
  }
  bytes = std::move(committed);
  return true;
}

static void test_nominal() {
  std::vector<uint8_t> bytes;
  if (!load_example("nominal", bytes)) return;

  DecodedRecording rec;
  rec.decode(bytes.data(), bytes.size());
  CHECK(rec.final_status() == FrameStatus::Ok);
  CHECK(rec.frame_count() == 19);

  CHECK(rec.sessions().sessions().size() == 1);
  CHECK(rec.sessions().diagnostics().empty());
  if (rec.sessions().sessions().size() == 1) {
    const Session& s = rec.sessions().sessions()[0];
    CHECK(s.device_id == 0x00A10001);
    CHECK(s.label == "drive-a1 bench run");
    CHECK(s.closed);
    CHECK(s.duration_ns() == 18000);
    CHECK(s.checkpoints.size() == 1);
    CHECK(s.markers.size() == 1);
  }

  CHECK(rec.schemas().count() == 2);
  CHECK(rec.schemas().total_versions() == 2);
  const ChannelSeries* motor = rec.channels().series(1);
  const ChannelSeries* bus = rec.channels().series(2);
  CHECK(motor != nullptr && bus != nullptr);
  if (motor) {
    CHECK(motor->schema().name == "motor");
    CHECK(motor->total_samples() == 6);
    CHECK(motor->block_count() == 3);
    CHECK(motor->stride() == 5);
  }
  if (bus) {
    CHECK(bus->total_samples() == 3);
    CHECK(bus->stride() == 8);
  }

  CHECK(rec.anomalies().faults().size() == 1);
  CHECK(rec.anomalies().transitions().size() == 4);
  CHECK(rec.anomalies().config_changes().size() == 1);
  CHECK(rec.anomalies().diagnostics().empty());
  if (rec.anomalies().faults().size() == 1) {
    const FaultEvent& f = rec.anomalies().faults()[0];
    CHECK(f.fault_code == 0x0104);
    CHECK(f.severity == Severity::Warning);
    CHECK(f.state_at_fault.value_or(0) == 2);
  }

  DeriveEngine engine;
  FrameStream fs(bytes.data(), bytes.size());
  Frame f;
  while (fs.next(f)) {
    if (f.header.type == RecordType::DerivedDef) {
      CHECK(engine.ingest_frame(f) == DeriveError::None);
    }
  }
  CHECK(engine.count() == 1);
  DerivedSeries series;
  CHECK(engine.evaluate(1, rec.channels(), series) == DeriveError::None);
  CHECK(series.values.size() == 6);
  if (series.values.size() == 6) {

    CHECK(series.values[0] > 1019.9 && series.values[0] < 1020.1);
  }

  ColumnStats stats;
  CHECK(compute_column_stats(rec.channels(), 1, 0, stats) == StatsError::None);
  CHECK(stats.count == 6);
  CHECK(stats.min == 1180);
  CHECK(stats.max == 1305);

  std::string csv;
  CHECK(export_channel_delimited(rec.channels(), rec.schemas(), 1,
                                 DelimFormat::Csv, csv) ==
        TableExportError::None);
  size_t lines = 0;
  for (char c : csv) {
    if (c == '\n') ++lines;
  }
  CHECK(lines == 7);
}

static void test_fleet() {
  std::vector<uint8_t> bytes;
  if (!load_example("fleet", bytes)) return;

  DecodedRecording rec;
  rec.decode(bytes.data(), bytes.size());
  CHECK(rec.final_status() == FrameStatus::Ok);
  CHECK(rec.frame_count() == 25);

  SessionSummary sum = rec.sessions().summary();
  CHECK(sum.device_count == 3);
  CHECK(sum.total_sessions == 3);
  CHECK(sum.closed_sessions == 2);
  CHECK(sum.unterminated_sessions == 1);
  CHECK(sum.total_reconnects == 1);
  CHECK(sum.total_checkpoints == 1);
  CHECK(sum.total_seq_regressions == 0);
  CHECK(sum.total_ts_regressions == 0);
  CHECK(rec.sessions().diagnostics().size() == 1);
  if (rec.sessions().diagnostics().size() == 1) {
    CHECK(rec.sessions().diagnostics()[0].code ==
          SessionDiagCode::UnterminatedSession);
  }

  CHECK(rec.schemas().count() == 2);
  CHECK(rec.schemas().versions(1).size() == 1);
  CHECK(rec.schemas().versions(2).size() == 2);
  const ChannelSchema* bus_v2 = rec.schemas().latest(2);
  CHECK(bus_v2 != nullptr);
  if (bus_v2) {
    CHECK(bus_v2->version == 2);
    CHECK(bus_v2->fields.size() == 3);
    CHECK(bus_v2->fields[2].name == "board_c");
  }

  const ChannelSeries* bus = rec.channels().series(2);
  CHECK(bus != nullptr);
  if (bus) {
    CHECK(bus->schema_version() == 1);
    CHECK(bus->total_samples() == 1);
  }

  CHECK(rec.anomalies().faults().size() == 1);
  if (rec.anomalies().faults().size() == 1) {
    CHECK(rec.anomalies().faults()[0].severity == Severity::Error);
    CHECK(rec.anomalies().faults()[0].device_id == 0x00A10002);
  }
}

static void test_degraded() {
  std::vector<uint8_t> bytes;
  if (!load_example("degraded", bytes)) return;

  DecodedRecording rec;
  rec.decode(bytes.data(), bytes.size());

  CHECK(rec.final_status() == FrameStatus::Incomplete);
  CHECK(rec.frame_count() == 17);

  bool saw_end_without_begin = false;
  bool saw_unterminated = false;
  for (const SessionDiagnostic& d : rec.sessions().diagnostics()) {
    if (d.code == SessionDiagCode::EndWithoutBegin) saw_end_without_begin = true;
    if (d.code == SessionDiagCode::UnterminatedSession) saw_unterminated = true;
  }
  CHECK(saw_end_without_begin);
  CHECK(saw_unterminated);

  bool saw_desync = false;
  for (const AnomalyDiagnostic& d : rec.anomalies().diagnostics()) {
    if (d.code == AnomalyDiagCode::StateDesync) saw_desync = true;
  }
  CHECK(saw_desync);
  bool desync_flagged = false;
  for (const TransitionEvent& t : rec.anomalies().transitions()) {
    if (!t.continues_chain) desync_flagged = true;
  }
  CHECK(desync_flagged);

  const std::vector<FaultEvent>& faults = rec.anomalies().faults();
  CHECK(faults.size() == 4);
  std::array<size_t, 4> by_sev = rec.anomalies().fault_count_by_severity();
  CHECK(by_sev[0] == 0);
  CHECK(by_sev[1] == 1);
  CHECK(by_sev[2] == 2);
  CHECK(by_sev[3] == 1);
  std::vector<BurstWindow> bursts = rec.anomalies().burst_windows(1000, 3);
  CHECK(bursts.size() == 1);
  if (bursts.size() == 1) {
    CHECK(bursts[0].count == 4);
    CHECK(bursts[0].device_id == 0x00B20007);
  }
}

static void test_render_determinism() {
  const char* names[] = {"nominal", "fleet", "degraded"};
  for (const char* name : names) {
    std::vector<uint8_t> bytes;
    if (!load_example(name, bytes)) continue;

    DecodedRecording rec;
    rec.decode(bytes.data(), bytes.size());
    const std::vector<Session>& sessions = rec.sessions().sessions();
    ReportModel model;
    model.sessions = &sessions;
    model.schemas = &rec.schemas();
    model.channels = &rec.channels();
    model.anomalies = &rec.anomalies();

    const ReportFormat formats[] = {ReportFormat::Text, ReportFormat::Json,
                                    ReportFormat::Dot};
    for (ReportFormat fmt : formats) {
      std::string a = render(model, fmt);
      std::string b = render(model, fmt);
      CHECK(a == b);
      CHECK(!a.empty());
    }

    QueryEngine engine(&sessions, &rec.channels(), &rec.anomalies(),
                       &rec.schemas());
    const char* queries[] = {"select sessions", "select faults",
                             "select transitions", "select channels"};
    for (const char* q : queries) {
      QueryResult result;
      QueryError err = engine.run(q, result);
      if (err != QueryError::None) {
        std::fprintf(stderr, "%s: query \"%s\" failed: %s\n", name, q,
                     query_error_str(err));
        ++failures;
      }
    }
  }
}

static void test_prefix_robustness() {
  const char* names[] = {"nominal", "fleet", "degraded"};
  for (const char* name : names) {
    std::vector<uint8_t> bytes;
    if (!load_example(name, bytes)) continue;
    for (size_t len = 0; len <= bytes.size(); ++len) {
      DecodedRecording rec;
      rec.decode(bytes.data(), len);
      const std::vector<Session>& sessions = rec.sessions().sessions();
      ReportModel model;
      model.sessions = &sessions;
      model.schemas = &rec.schemas();
      model.channels = &rec.channels();
      model.anomalies = &rec.anomalies();
      std::string text = render(model, ReportFormat::Text);
      CHECK(!text.empty());
    }
  }
}

int main() {
  test_nominal();
  test_fleet();
  test_degraded();
  test_render_determinism();
  test_prefix_robustness();

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("test_examples: all checks passed\n");
  return 0;
}
