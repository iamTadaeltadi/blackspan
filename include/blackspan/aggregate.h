#ifndef BLACKSPAN_AGGREGATE_H
#define BLACKSPAN_AGGREGATE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/channel.h"

// The aggregate subsystem summarizes a channel's samples over fixed windows of
// device-monotonic time. A recorder may sample a channel far faster than a
// report needs to show, so triage wants "the mean bus voltage per second" or
// "the peak current per 100 ms" rather than every raw sample. This subsystem
// produces those: it reads a channel field's samples with the timestamp each
// inherits from its block, groups them into contiguous windows of a chosen
// width, and reduces each window with a chosen function.
//
// It reads the channel store only; it mutates nothing and never touches raw
// bytes. Every routine is a pure function of its inputs. Nothing here throws;
// degenerate inputs (an unknown channel, a zero window) yield an empty result.

namespace blackspan {

// The reduction applied within a window.
enum class AggFn : uint8_t {
  Mean = 0,   // arithmetic mean of the window's values
  Min = 1,    // smallest value
  Max = 2,    // largest value
  First = 3,  // earliest value in the window
  Last = 4,   // latest value in the window
  Count = 5,  // number of samples in the window
  Sum = 6,    // sum of the window's values
};

// Lowercase name of an aggregation function ("mean", "min", ...). Never null.
const char* agg_fn_name(AggFn f);

// Parses an aggregation-function name, returning false for an unknown one.
bool parse_agg_fn(const std::string& s, AggFn& out);

// One aggregated window: the window's start timestamp (a multiple of the window
// width measured from the first sample), the number of samples it covered, and
// the reduced value. An empty window is never emitted.
struct AggBucket {
  uint64_t window_start = 0;
  size_t count = 0;
  double value = 0.0;
};

// Reads a channel field's samples with their timestamps: each returned pair is
// (timestamp, value), where the timestamp is the base timestamp of the block the
// sample belongs to. Samples that cannot be read are skipped. Returns an empty
// vector for an unknown channel or an out-of-range field. Pairs are in sample
// (ingest) order, which is non-decreasing in timestamp for a well-formed
// recording.
std::vector<std::pair<uint64_t, double>> column_with_timestamps(
    const ChannelStore& store, uint16_t channel_id, size_t field_index);

// Aggregates a channel field into contiguous windows of `window_ns`, reducing
// each with `fn`. Windows are aligned to the first sample's timestamp: a sample
// at time t falls in window floor((t - t0) / window_ns). Only non-empty windows
// are returned, in ascending time order. A window_ns of 0, an unknown channel,
// or a field with no readable samples yields an empty vector.
std::vector<AggBucket> aggregate_channel(const ChannelStore& store,
                                         uint16_t channel_id,
                                         size_t field_index, uint64_t window_ns,
                                         AggFn fn);

// Aggregates an already-extracted set of (timestamp, value) pairs, which the
// caller may have calibrated first. The pairs need not be sorted; they are
// bucketed by timestamp relative to the minimum timestamp present.
std::vector<AggBucket> aggregate_series(
    const std::vector<std::pair<uint64_t, double>>& samples, uint64_t window_ns,
    AggFn fn);

// Renders a set of buckets to a deterministic multi-line block: a header line
// with the window count, then one line per bucket. An empty input renders a
// single "0 windows" line.
std::string render_buckets(const std::vector<AggBucket>& buckets, AggFn fn);

}  // namespace blackspan

#endif  // BLACKSPAN_AGGREGATE_H
