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
#include "blackspan/stats.h"

using namespace blackspan;

namespace {

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
    g_sink += series->keyframe_sample_indices().size();
    g_sink += series->keyframe_timestamps().size();
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
  for (uint32_t seq : session_resume_anchor_seqs(sessions.sessions())) {
    g_sink += seq;
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

  std::vector<HealthFinding> findings = channel_health_scan(channels, 1.0);
  for (const HealthFinding& f : findings) {
    g_sink += f.samples;
    g_sink += describe_health(f).size();
    g_sink += static_cast<size_t>(f.flagged);
  }
  for (uint16_t id : channels.channels()) {
    g_sink += static_cast<size_t>(smoothness_index(channels.column_as_doubles(id, 0)));
  }
}

}

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
