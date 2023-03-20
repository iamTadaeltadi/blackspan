// decode: drives the whole reconstruction pipeline from one recording buffer.
//
// A single DecodedRecording::decode runs the frame stream and routes every
// frame to the schema registry, the session reconstructor, the channel column
// stores, and the anomaly engine. The harness then reads the reconstructed
// model end to end -- every channel's samples through the typed view, every
// session and anomaly through its describe path, burst detection, and severity
// tallies -- so lazily built and cross-subsystem read paths all execute on the
// decoded state rather than sitting untouched behind the decode call.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/channel.h"
#include "blackspan/health.h"
#include "blackspan/pipeline.h"
#include "blackspan/schema.h"
#include "blackspan/session.h"

using namespace blackspan;

namespace {

// Prevents the compiler from discarding the read-only walk below.
volatile size_t g_sink = 0;

void walk_channels(const ChannelStore& channels) {
  g_sink += channels.total_samples();
  for (uint16_t id : channels.channels()) {
    const ChannelSeries* series = channels.series(id);
    if (!series) continue;
    g_sink += series->total_samples();
    g_sink += series->stride();
    g_sink += series->block_count();
    g_sink += channels.retained_samples(id);
    const size_t fields = series->schema().fields.size();
    for (size_t f = 0; f < fields; ++f) {
      std::vector<double> col = channels.column_as_doubles(id, f);
      g_sink += col.size();
    }
    for (size_t b = 0; b < series->block_count(); ++b) {
      g_sink += series->block_sample_count(b);
      g_sink += series->block_base_ts(b);
    }
    for (size_t i = 0; i < series->total_samples(); ++i) {
      std::optional<SampleView> v = series->view(i);
      if (!v.has_value()) continue;
      for (size_t f = 0; f < v->field_count(); ++f) {
        std::optional<double> d = v->get_as_double(f);
        if (d.has_value()) g_sink += static_cast<size_t>(*d != 0.0);
      }
    }
  }
}

void walk_sessions(const SessionReconstructor& sessions) {
  for (const Session& s : sessions.sessions()) {
    std::string text = describe_session(s);
    g_sink += text.size();
    g_sink += static_cast<size_t>(s.duration_ns());
  }
  g_sink += sessions.diagnostics().size();
}

void walk_anomalies(const AnomalyEngine& anomalies) {
  for (const FaultEvent& f : anomalies.faults()) {
    g_sink += describe_fault(f).size();
  }
  for (const TransitionEvent& t : anomalies.transitions()) {
    g_sink += describe_transition(t).size();
  }
  for (const ConfigEvent& c : anomalies.config_changes()) {
    g_sink += describe_config(c).size();
  }
  std::array<size_t, 4> by_sev = anomalies.fault_count_by_severity();
  for (size_t n : by_sev) g_sink += n;
  std::vector<BurstWindow> bursts = anomalies.burst_windows(1000000, 2);
  g_sink += bursts.size();
  g_sink += describe_anomaly_summary(anomalies.summary()).size();
}

void walk_health(const ChannelStore& channels) {
  // Score every channel's restlessness the way triage would, exercising the
  // length-proportional baseline over the decoded columns.
  std::vector<HealthFinding> findings = channel_health_scan(channels, 1.0);
  for (const HealthFinding& f : findings) {
    g_sink += f.samples;
    g_sink += describe_health(f).size();
    g_sink += static_cast<size_t>(f.flagged);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  DecodedRecording rec;
  rec.decode(data, size);

  g_sink += rec.frame_count();
  g_sink += static_cast<size_t>(rec.final_status());

  walk_channels(rec.channels());
  walk_sessions(rec.sessions());
  walk_anomalies(rec.anomalies());
  walk_health(rec.channels());
  return 0;
}
