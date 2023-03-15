#ifndef BLACKSPAN_RESAMPLE_H
#define BLACKSPAN_RESAMPLE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/channel.h"

// The resample subsystem places an irregular (timestamp, value) sample series
// onto a uniform time grid. A recorder samples each channel on its own clock --
// one at a fixed rate, another only when a value changes, a third in bursts --
// so two channels that triage wants to compare or combine rarely share sample
// instants. Resampling reconstructs a value for every channel at the same grid
// of timestamps, which is the precondition for differencing, correlating, or
// deriving one channel from another.
//
// The core operates on a plain vector of (timestamp, value) pairs assumed
// ascending by timestamp, so it is fully testable without a recording; a thin
// adapter pulls the same block-timestamped column the aggregate subsystem reads
// and feeds it through the core. Every routine is a pure function of its
// inputs, mutates nothing, and never throws. A value the grid cannot define --
// a point outside the series' span, or one separated from its supporting
// samples by more than the caller's gap bound -- is reported as NaN rather than
// invented.

namespace blackspan {

// How a grid point's value is drawn from the surrounding samples.
enum class ResampleMethod : uint8_t {
  Nearest = 0,       // value of whichever bracketing sample is closer in time
  PreviousHold = 1,  // value of the last sample at or before the grid point
  Linear = 2,        // linear interpolation between the two bracketing samples
};

// Lowercase name of a method ("nearest", "hold", "linear"). Never null.
const char* resample_method_name(ResampleMethod m);

// Parses a method name, returning false for an unknown one.
bool parse_resample_method(const std::string& s, ResampleMethod& out);

// A uniform output grid: `count` points at start, start+step, ...,
// start+(count-1)*step. A step of 0 or a count of 0 describes an empty grid.
struct ResampleGrid {
  uint64_t start = 0;
  uint64_t step = 1;
  size_t count = 0;

  // The timestamp of grid point i (i in [0, count)); no bounds check.
  uint64_t at(size_t i) const { return start + static_cast<uint64_t>(i) * step; }
};

// Builds a grid spanning [first_ts, last_ts] of `in` at the given step, deriving
// the point count from the span. Returns a zero-count grid for an empty series
// or a zero step. The span is taken over the minimum and maximum timestamps
// present, so an out-of-order series still yields the same grid as its sorted
// form.
ResampleGrid grid_from_series(const std::vector<std::pair<uint64_t, double>>& in,
                              uint64_t step);

// A resampling request: the method, the grid to sample onto, and an optional
// gap bound.
struct ResamplePlan {
  ResampleMethod method = ResampleMethod::Linear;
  ResampleGrid grid;

  // A grid point farther than max_gap_ns from the samples that support it yields
  // NaN rather than a held or interpolated value, so a long recording gap is not
  // silently bridged. For Nearest and PreviousHold the distance measured is from
  // the grid point to the chosen sample; for Linear it is the width of the
  // bracketing interval. A value of 0 disables the check.
  uint64_t max_gap_ns = 0;
};

// The result of resampling: one timestamp and one value per grid point, plus a
// tally of how many outputs are defined versus NaN. `ts` and `values` always
// have size grid.count, and defined + nan == grid.count.
struct ResampledSeries {
  std::vector<uint64_t> ts;    // grid timestamps (size == grid.count)
  std::vector<double> values;  // resampled values (NaN where undefined)
  size_t defined = 0;          // count of non-NaN outputs
  size_t nan = 0;              // count of NaN outputs
};

// Resamples `in` onto plan.grid. `in` is assumed ascending by timestamp; if it
// is not sorted the function resamples a stably sorted local copy instead.
// Values at grid points before the first sample or after the last are NaN. Ties
// (equal timestamps) resolve to the last sample recorded at that timestamp.
// Never throws.
ResampledSeries resample_series(const std::vector<std::pair<uint64_t, double>>& in,
                                const ResamplePlan& plan);

// ChannelStore adapter: reads (block_base_ts, value) pairs for a channel field
// -- the same block-timestamped column the aggregate subsystem reads -- then
// resamples them onto plan.grid. Returns a series honoring plan.grid with every
// value NaN for an unknown channel or an out-of-range field. This overload
// selects the field by its index in the channel schema.
ResampledSeries resample_channel(const ChannelStore& store, uint16_t channel_id,
                                 size_t field_index, const ResamplePlan& plan);

// As above, selecting the field by name. Returns an all-NaN series honoring
// plan.grid when the channel is unknown or names no such field.
ResampledSeries resample_channel(const ChannelStore& store, uint16_t channel_id,
                                 const std::string& field_name,
                                 const ResamplePlan& plan);

}  // namespace blackspan

#endif  // BLACKSPAN_RESAMPLE_H
