#ifndef BLACKSPAN_REWRITE_H
#define BLACKSPAN_REWRITE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "blackspan/frame.h"

namespace blackspan {

struct RewriteFilter {

  std::optional<uint32_t> device;

  std::optional<uint64_t> since;
  std::optional<uint64_t> until;

  std::optional<uint32_t> seq_lo;
  std::optional<uint32_t> seq_hi;

  std::vector<RecordType> types;

  bool keep(const FrameHeader& h) const;
};

struct RewriteResult {
  std::vector<uint8_t> bytes;
  size_t frames_in = 0;
  size_t frames_kept = 0;
  FrameStatus input_status = FrameStatus::Ok;
};

RewriteResult rewrite_slice(const uint8_t* data, size_t len,
                            const RewriteFilter& filter);

RewriteResult rewrite_slice(const std::vector<uint8_t>& data,
                            const RewriteFilter& filter);

struct MergeResult {
  std::vector<uint8_t> bytes;
  size_t frames_out = 0;
  size_t inputs_truncated = 0;
};

MergeResult rewrite_merge(const std::vector<std::vector<uint8_t>>& inputs);

}

#endif
