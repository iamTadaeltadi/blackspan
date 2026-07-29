#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/digest.h"
#include "blackspan/driftmodel.h"
#include "blackspan/blob.h"
#include "blackspan/eventgraph.h"
#include "blackspan/fault_taxonomy.h"
#include "blackspan/journal.h"
#include "blackspan/pipeline.h"
#include "blackspan/profile.h"
#include "blackspan/report.h"
#include "blackspan/session.h"
#include "blackspan/stats.h"

using namespace blackspan;

namespace {
volatile size_t g_sink = 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  DecodedRecording rec;
  rec.decode(data, size);

  const std::vector<Session>& sessions = rec.sessions().sessions();
  ReportModel model;
  model.sessions = &sessions;
  model.schemas = &rec.schemas();
  model.channels = &rec.channels();
  model.anomalies = &rec.anomalies();

  uint16_t mask = size ? static_cast<uint16_t>(data[size - 1]) : kSectionAll;

  g_sink += render_text(model, mask).size();
  g_sink += render_json(model, mask).size();
  g_sink += render_dot(model, mask).size();
  g_sink += render(model, ReportFormat::Text, kSectionAll).size();

  for (const ChannelDigest& d : channel_digests(rec.channels())) {
    g_sink += d.samples;
    g_sink += d.buckets.size();
    g_sink += describe_digest(d).size();
  }

  for (const ChannelDrift& d : channel_drift(rec)) {
    g_sink += d.samples;
    g_sink += describe_drift(d).size();
  }

  StateGraph sg = recording_state_graph(rec);
  if (!sg.nodes.empty()) {
    g_sink += graph_eccentricity(sg, sg.nodes.front().state);
  }

  CodeTally fault_total;
  for (const CodeTally& t : tally_by_code(rec.anomalies().faults(), fault_total)) {
    g_sink += t.count;
  }
  g_sink += fault_total.count;

  for (float e : channel_annotation_energy(rec.channels())) {
    g_sink += static_cast<size_t>(e);
  }

  g_sink += static_cast<size_t>(fault_detail_footprint(rec.anomalies().faults()));

  for (const ActivityProfile& p : channel_activity(rec.channels())) {
    g_sink += p.samples;
    g_sink += describe_activity(p).size();
    g_sink += static_cast<size_t>(activity_deviation(p));
  }

  if (size >= kExportRequestSize) {
    ExportRequest req;
    ExportError err =
        decode_export_request(data + size - kExportRequestSize,
                              kExportRequestSize, req);
    if (err == ExportError::None) {
      g_sink += render(model, req).size();
    }
  }
  return 0;
}
