// assemble: parses a recording script and decodes what it encodes.
//
// The whole input is treated as script text (the .bsr language the CLI's
// assemble command accepts). assemble_script lexes and encodes it into a
// recording; on success the harness decodes those bytes back through the full
// pipeline, so the encoder and decoder are exercised as a round trip and any
// frame the assembler can emit is guaranteed to be one the decoder accepts.
#include <cstddef>
#include <cstdint>
#include <string>

#include "blackspan/assemble.h"
#include "blackspan/pipeline.h"

using namespace blackspan;

namespace {
volatile size_t g_sink = 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string text(reinterpret_cast<const char*>(data), size);

  AssembleResult result = assemble_script(text);
  g_sink += result.errors.size();

  if (result.ok()) {
    g_sink += result.frame_count;
    DecodedRecording rec;
    rec.decode(result.bytes.data(), result.bytes.size());
    g_sink += rec.frame_count();
    g_sink += rec.channels().total_samples();
    g_sink += rec.sessions().sessions().size();
    g_sink += rec.anomalies().faults().size();
  }
  return 0;
}
