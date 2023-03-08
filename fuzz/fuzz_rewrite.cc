// rewrite: slices and merges a recording, then decodes the results.
//
// A filter is built from the input's leading byte (a record-type bitmask); the
// remainder is sliced under it and re-encoded, then decoded back through the
// full pipeline so the slice output is validated as a real recording. The
// original and the slice are also merged into one ordered stream and decoded.
// This drives the frame re-encoder and the merge ordering over arbitrary input.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "blackspan/frame.h"
#include "blackspan/pipeline.h"
#include "blackspan/rewrite.h"

using namespace blackspan;

namespace {
volatile size_t g_sink = 0;

size_t decode_and_count(const std::vector<uint8_t>& bytes) {
  DecodedRecording rec;
  rec.decode(bytes.data(), bytes.size());
  return rec.frame_count() + rec.channels().total_samples();
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  uint8_t selector = size ? data[0] : 0xFF;
  const uint8_t* body = size ? data + 1 : data;
  size_t body_len = size ? size - 1 : 0;

  RewriteFilter filter;
  // Bits 0..11 select record types 0x01..0x0C; an all-zero selector keeps all.
  for (uint8_t i = 0; i < 12; ++i) {
    if (selector & (1u << (i & 7))) {
      filter.types.push_back(static_cast<RecordType>(i + 1));
    }
  }

  RewriteResult sliced = rewrite_slice(body, body_len, filter);
  g_sink += sliced.frames_in;
  g_sink += sliced.frames_kept;
  g_sink += decode_and_count(sliced.bytes);

  std::vector<std::vector<uint8_t>> inputs;
  inputs.emplace_back(body, body + body_len);
  inputs.push_back(sliced.bytes);
  MergeResult merged = rewrite_merge(inputs);
  g_sink += merged.frames_out;
  g_sink += merged.inputs_truncated;
  g_sink += decode_and_count(merged.bytes);
  return 0;
}
