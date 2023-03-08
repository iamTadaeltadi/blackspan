// derive: parses derived-channel formulas and evaluates them over a recording.
//
// The input splits into a recording and a formula string. The recording is
// decoded for its channel store, and every derived-def frame it carries is fed
// through the derived-def payload decoder and registered in a DeriveEngine,
// exercising the expression parser at registration. The trailing string is also
// registered as a formula against the first channel, so arbitrary expression
// text reaches the parser directly. Finally every registered definition is
// evaluated over the store, running the expression VM across real samples.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "blackspan/channel.h"
#include "blackspan/derive.h"
#include "blackspan/expr.h"
#include "blackspan/frame.h"
#include "blackspan/pipeline.h"
#include "fuzz_split.h"

using namespace blackspan;

namespace {
volatile size_t g_sink = 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  SplitInput in = split_input(data, size);

  DecodedRecording rec;
  rec.decode(in.recording.data(), in.recording.size());
  const ChannelStore& store = rec.channels();

  DeriveEngine engine;

  // Register every derived-def frame the recording carries.
  FrameStream stream(in.recording.data(), in.recording.size());
  Frame frame;
  while (stream.next(frame)) {
    if (frame.header.type != RecordType::DerivedDef) continue;
    DerivedDefRecord decoded;
    DeriveError derr =
        decode_derived_def(frame.payload.data(), frame.payload.size(), decoded);
    g_sink += static_cast<size_t>(derr);
    ExprError perr = ExprError::None;
    engine.ingest_frame(frame, &perr);
  }

  // Drive the expression parser directly with the trailing text.
  std::vector<uint16_t> channel_ids = store.channels();
  DerivedDefRecord textual;
  textual.derived_id = 0xF000;
  textual.primary_channel = channel_ids.empty() ? 0 : channel_ids.front();
  textual.name = "fuzz";
  textual.expr = in.text;
  ExprError parse_err = ExprError::None;
  size_t parse_pos = 0;
  engine.register_def(textual, &parse_err, &parse_pos);

  // Evaluate everything that registered.
  for (uint16_t id : engine.defs()) {
    DerivedSeries out;
    DeriveError eerr = engine.evaluate(id, store, out);
    if (eerr == DeriveError::None) {
      g_sink += out.values.size();
      g_sink += out.timestamps.size();
    }
  }
  return 0;
}
