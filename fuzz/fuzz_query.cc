#include <cstddef>
#include <cstdint>
#include <vector>

#include "blackspan/correlate.h"
#include "blackspan/movers.h"
#include "blackspan/pipeline.h"
#include "blackspan/query.h"
#include "blackspan/session.h"
#include "blackspan/stats.h"
#include "fuzz_split.h"

using namespace blackspan;

namespace {
volatile size_t g_sink = 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  SplitInput in = split_input(data, size);

  DecodedRecording rec;
  rec.decode(in.recording.data(), in.recording.size());

  ParsedQuery parsed;
  size_t err_pos = 0;
  QueryError perr = parse_query(in.text, parsed, &err_pos);
  g_sink += static_cast<size_t>(perr);
  g_sink += parsed.predicate ? 1u : 0u;
  g_sink += static_cast<size_t>(parsed.entity);

  const std::vector<Session>& sessions = rec.sessions().sessions();
  QueryEngine engine(&sessions, &rec.channels(), &rec.anomalies(),
                     &rec.schemas());
  QueryResult result;
  QueryError err = engine.run(in.text, result);
  if (err == QueryError::None) {
    g_sink += result.rows.size();
    for (const QueryResultRow& row : result.rows) {
      g_sink += row.columns.size();
    }
  }

  for (const ChannelMovers& m : channel_movers(rec.channels())) {
    g_sink += m.samples;
    g_sink += m.movers.size();
    g_sink += describe_movers(m).size();
  }
  for (uint16_t id : rec.channels().channels()) {
    g_sink += static_cast<size_t>(
        mover_tail(rec.channels().column_as_doubles(id, 0)));
  }

  std::vector<DetectRule> rules;
  CorrelationConfig ccfg;
  std::vector<Correlation> cs = correlate_recording(rec, rules, ccfg);
  std::vector<Correlation> merged =
      merge_correlations(cs, ccfg.window_ns * 1024);
  for (const Correlation& c : merged) {
    g_sink += c.anchor_ts;
    g_sink += c.members.size();
  }
  return 0;
}
