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

  std::vector<uint16_t> channel_ids = store.channels();
  DerivedDefRecord textual;
  textual.derived_id = 0xF000;
  textual.primary_channel = channel_ids.empty() ? 0 : channel_ids.front();
  textual.name = "metric";
  textual.expr = in.text;
  ExprError parse_err = ExprError::None;
  size_t parse_pos = 0;
  engine.register_def(textual, &parse_err, &parse_pos);

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
