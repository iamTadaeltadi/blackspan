#ifndef BLACKSPAN_JOIN_H
#define BLACKSPAN_JOIN_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/channel.h"

// The join subsystem aligns two channel-field series onto shared rows so that
// two quantities a recorder sampled on different clocks can be compared or
// combined. A recorder rarely samples two channels at the same instants -- one
// runs at a fixed rate, another only when its value changes -- so a report that
// wants "bus current at each voltage reading" cannot simply zip the two columns
// index by index. This subsystem produces the aligned rows: an exact-timestamp
// inner join, or an as-of join that carries the most recent (or earliest)
// sample from the right series forward (or backward) to each left sample.
//
// The core operates on plain vectors of ascending (timestamp, value) pairs, so
// it is fully testable without a recording; a thin adapter pulls the same
// block-timestamped column the aggregate and resample subsystems read and feeds
// two of them through the core. Every routine is a pure function of its inputs,
// mutates nothing, and never throws. Inputs are treated as ascending by
// timestamp; an input that is not sorted is joined against a stably sorted local
// copy instead, so equal-timestamp ties keep their ingest order.

namespace blackspan {

// How two series are aligned onto rows.
enum class JoinKind : uint8_t {
  Inner,         // emit a row only where both series have a sample at one ts
  AsOfBackward,  // pair each left sample with the most recent right sample at
                 // ts <= left.ts, within tolerance
  AsOfForward,   // pair each left sample with the earliest right sample at
                 // ts >= left.ts, within tolerance
};

// Lowercase name of a join kind ("inner", "asof-backward", "asof-forward").
// Never null.
const char* join_kind_name(JoinKind k);

// Parses a join-kind name, returning false for an unknown one.
bool parse_join_kind(const std::string& s, JoinKind& out);

// One aligned row. `ts` is the row's timestamp: the shared timestamp for an
// inner join, or the left sample's timestamp for an as-of join. `left` and
// `right` are the paired values, and `right_ts` is the timestamp of the right
// sample that supplied `right` (equal to `ts` for an inner join, and the
// as-of-matched right timestamp otherwise).
struct JoinedRow {
  uint64_t ts = 0;
  double left = 0.0;
  double right = 0.0;
  uint64_t right_ts = 0;
};

// The result of a join: the aligned rows in ascending row order, plus tallies of
// the samples that produced no row.
//
// For an inner join each side is first reduced to one value per timestamp (the
// last value recorded at a shared timestamp wins), and the unmatched counts are
// of the distinct timestamps on each side that had no partner on the other.
//
// For an as-of join every left sample is considered in turn: left_unmatched
// counts the left samples with no qualifying right sample within tolerance, and
// right_unmatched counts the right samples never selected as any left sample's
// partner.
struct JoinResult {
  std::vector<JoinedRow> rows;
  size_t left_unmatched = 0;
  size_t right_unmatched = 0;
};

// Joins two ascending (timestamp, value) series.
//
// Inner ignores `tolerance_ns` and pairs only samples that share a timestamp,
// via a two-pointer merge over the last-value-per-timestamp reduction of each
// side. As-of joins binary-search the right series for the nearest qualifying
// sample and record its timestamp in each row; `tolerance_ns` bounds, inclusively,
// how far in time an as-of match may reach (0 = unbounded). When several right
// samples share the matched timestamp, AsOfBackward takes the last recorded and
// AsOfForward the first, matching the "most recent"/"earliest" wording.
//
// Both inputs are treated as ascending; a defensive stable sort of a local copy
// is applied to an input that is not sorted. Never throws.
JoinResult join_series(const std::vector<std::pair<uint64_t, double>>& left,
                       const std::vector<std::pair<uint64_t, double>>& right,
                       JoinKind kind, uint64_t tolerance_ns);

// ChannelStore adapter: reads each (channel, field) as (block_base_ts, value)
// pairs -- the same block-timestamped column the aggregate and resample
// subsystems read -- then joins the two. An unknown channel or an out-of-range
// field contributes an empty series on its side, so the join still runs and the
// present side's samples fall entirely into the unmatched tallies. Fields are
// selected by their index in the channel schema.
JoinResult join_channels(const ChannelStore& store, uint16_t left_channel,
                         size_t left_field, uint16_t right_channel,
                         size_t right_field, JoinKind kind,
                         uint64_t tolerance_ns);

}  // namespace blackspan

#endif  // BLACKSPAN_JOIN_H
