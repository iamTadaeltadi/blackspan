#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/export.h"
#include "blackspan/pipeline.h"
#include "blackspan/rank.h"
#include "blackspan/session.h"

using namespace blackspan;

namespace {
volatile size_t g_sink = 0;

void export_channel(const ChannelStore& channels, const SchemaRegistry& schemas,
                    uint16_t id) {
  std::string out;
  export_channel_delimited(channels, schemas, id, DelimFormat::Csv, out);
  g_sink += out.size();
  export_channel_delimited(channels, schemas, id, DelimFormat::Tsv, out);
  g_sink += out.size();
  export_channel_ndjson(channels, schemas, id, out);
  g_sink += out.size();
}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  DecodedRecording rec;
  rec.decode(data, size);

  const ChannelStore& channels = rec.channels();
  const SchemaRegistry& schemas = rec.schemas();
  const AnomalyEngine& anomalies = rec.anomalies();

  for (uint16_t id : channels.channels()) {
    export_channel(channels, schemas, id);
  }

  for (const ChannelRanks& r : channel_ranks(channels)) {
    g_sink += r.samples;
    g_sink += r.ranks.size();
    g_sink += describe_ranks(r).size();
  }

  std::string out;
  export_faults_delimited(anomalies, DelimFormat::Csv, out);
  g_sink += out.size();
  export_transitions_delimited(anomalies, DelimFormat::Tsv, out);
  g_sink += out.size();
  export_sessions_delimited(rec.sessions().sessions(), DelimFormat::Csv, out);
  g_sink += out.size();
  export_faults_ndjson(anomalies, out);
  g_sink += out.size();
  return 0;
}
