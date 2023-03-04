#ifndef BLACKSPAN_REWRITE_H
#define BLACKSPAN_REWRITE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "blackspan/frame.h"

// The rewrite subsystem produces new recordings from existing ones without
// leaving the wire format. Two operations live here. A slice keeps the frames of
// one recording that match a filter (a device, a record-type set, a time or
// sequence window) and re-emits them as a fresh, fully valid recording; it is
// how an engineer extracts one unit's frames from a multi-device download, drops
// the sample traffic to share just the control plane, or clips a recording to
// the window around an incident. A merge interleaves several recordings into one
// stream ordered by device-monotonic time, which is how per-visit downloads are
// combined into a single archive.
//
// Every frame the subsystem emits is re-encoded through the frame codec, so the
// header and payload CRCs of the output are always correct even when the input
// carried a corrupt frame (a corrupt frame simply ends that input's contribution
// at the point decoding could no longer proceed). The subsystem reads and writes
// bytes only; it does not build the higher-level model. Nothing here throws.

namespace blackspan {

// Selects which frames a slice keeps. An unset optional field imposes no
// constraint on that dimension; the constraints that are set are ANDed together.
struct RewriteFilter {
  // Keep only frames from this device id.
  std::optional<uint32_t> device;
  // Keep only frames whose monotonic_ts is within these inclusive bounds.
  std::optional<uint64_t> since;
  std::optional<uint64_t> until;
  // Keep only frames whose session_seq is within these inclusive bounds.
  std::optional<uint32_t> seq_lo;
  std::optional<uint32_t> seq_hi;
  // When non-empty, keep only frames whose record type is in this set. An empty
  // set keeps every record type.
  std::vector<RecordType> types;

  // True when this filter would keep the given frame header.
  bool keep(const FrameHeader& h) const;
};

// The outcome of a slice: the re-encoded bytes, how many complete frames the
// input held, how many the filter kept, and the terminal status of the input
// frame stream (Ok when fully consumed, Incomplete for a trailing partial
// frame, or the hard error that halted decoding — a slice stops at the first
// non-Ok frame just as the decoder does).
struct RewriteResult {
  std::vector<uint8_t> bytes;
  size_t frames_in = 0;
  size_t frames_kept = 0;
  FrameStatus input_status = FrameStatus::Ok;
};

// Slices data[0, len) under `filter`, re-encoding each kept frame. The output is
// a valid recording whose frames appear in their original order.
RewriteResult rewrite_slice(const uint8_t* data, size_t len,
                            const RewriteFilter& filter);

// Convenience overload over a byte vector.
RewriteResult rewrite_slice(const std::vector<uint8_t>& data,
                            const RewriteFilter& filter);

// The outcome of a merge: the interleaved bytes, the total frames emitted, and
// how many of the inputs stopped early on a corrupt or partial frame (those
// inputs contribute only the frames that decoded before the stop).
struct MergeResult {
  std::vector<uint8_t> bytes;
  size_t frames_out = 0;
  size_t inputs_truncated = 0;
};

// Merges several recordings into one stream ordered by (monotonic_ts, then input
// index, then original position within that input), so that frames sharing a
// timestamp keep a deterministic order and a single input's internal order is
// never reversed. Each input is decoded independently; a corrupt frame ends that
// input's contribution without affecting the others.
MergeResult rewrite_merge(const std::vector<std::vector<uint8_t>>& inputs);

}  // namespace blackspan

#endif  // BLACKSPAN_REWRITE_H
