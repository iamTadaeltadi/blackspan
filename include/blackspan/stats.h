#ifndef BLACKSPAN_STATS_H
#define BLACKSPAN_STATS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/channel.h"

// The stats subsystem computes summary statistics over the numeric data the
// decoder recovers: the per-channel sample columns produced by the channel
// subsystem and the fault timeline produced by the anomaly subsystem. It reads
// those subsystems' typed outputs and never touches the raw byte stream. Every
// routine is a pure function of its inputs; nothing here mutates the stores it
// reads, allocates beyond its result, or throws. Empty and degenerate inputs
// have documented sentinels (a NaN result, an invalid-stats flag, or a specific
// StatsError) so a caller never divides by zero or reads an undefined value.
//
// Two flavours of statistic live here. Column statistics summarize a channel's
// sample column as a whole (count, min/max, mean, population standard
// deviation) and derive shaped views of it (percentiles, histograms,
// downsampling, and rate-of-change). Event statistics summarize the fault
// stream (fault rate, inter-fault spacing, and a severity breakdown).

namespace blackspan {

// Error codes for the stats routines that can fail for a structural reason.
// Errors are values; nothing in this subsystem throws. The value-shaped
// routines (percentile, histogram, downsample) report degeneracy through their
// results instead and do not use this enum.
enum class StatsError : uint8_t {
  None = 0,
  UnknownChannel,  // compute_column_stats: the channel has no series
  BadField,        // the field index is out of range for the channel's schema
  EmptyColumn,     // the channel exists but the column has no readable samples
  LengthMismatch,  // rate_of_change: values and timestamps differ in length
};

// Human-readable form of a StatsError. Never null.
const char* stats_error_str(StatsError e);

// Summary statistics for one numeric column. `stddev` is the population
// standard deviation (divides by count, not count-1). `valid` is false for an
// empty column, in which case every numeric member is 0 and must be ignored.
struct ColumnStats {
  size_t count = 0;
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  double stddev = 0.0;
  double sum = 0.0;
  bool valid = false;
};

// Computes summary statistics over a vector of samples. An empty input yields a
// ColumnStats with valid == false and all numeric members 0. Otherwise every
// member is filled and valid is true; the standard deviation is the population
// standard deviation computed with a stable two-pass sum of squared deviations.
ColumnStats compute_stats(const std::vector<double>& values);

// Computes summary statistics for one field of one channel by reading the
// column through ChannelStore::column_as_doubles and summarizing it. Returns
// UnknownChannel if the channel has no series, BadField if field_index is out
// of range for the channel's schema, or EmptyColumn if the column yielded no
// readable samples (in which case `out.valid` is false). On None, `out` holds
// the computed statistics.
StatsError compute_column_stats(const ChannelStore& store, uint16_t channel_id,
                                size_t field_index, ColumnStats& out);

// The q-th percentile (q in [0, 100]) of `values` by linear interpolation on a
// sorted copy: rank = q/100 * (n - 1), interpolating between the two nearest
// order statistics. p0 is the minimum, p100 the maximum, p50 the median. q is
// clamped to [0, 100]. An empty input, or a NaN q, yields NaN. The argument is
// taken by value because it is sorted in place.
double percentile(std::vector<double> values, double q);

// The median (50th percentile). Empty input yields NaN.
double median(std::vector<double> values);

// A fixed-width histogram over [lo, hi]. `bucket_width` is (hi - lo) /
// bucket_count. `buckets[k]` counts values in the half-open range [lo + k*w,
// lo + (k+1)*w), except that a value exactly equal to hi is counted in the last
// bucket. `below` counts values less than lo, `above` counts values greater
// than hi. A degenerate range (hi <= lo) reports a zero bucket_width and routes
// every in-range value into bucket 0.
struct Histogram {
  double lo = 0.0;
  double hi = 0.0;
  double bucket_width = 0.0;
  std::vector<size_t> buckets;
  size_t below = 0;
  size_t above = 0;
};

// Builds a histogram of `values` with `bucket_count` buckets. When `lo` and/or
// `hi` are supplied they fix that edge of the range; an unsupplied edge is
// taken from the data's min/max (both unsupplied over an empty input gives a
// degenerate [0, 0] range). A bucket_count of 0 yields an empty histogram.
// Values below lo or above hi are tallied into `below` / `above` rather than
// dropped, so counts always sum to values.size().
Histogram histogram(const std::vector<double>& values, size_t bucket_count,
                    std::optional<double> lo = std::nullopt,
                    std::optional<double> hi = std::nullopt);

// Averages consecutive runs of `factor` samples into one value each. The final
// run is averaged even when shorter than `factor`. A factor of 0, or an empty
// input, yields an empty vector. The result has ceil(size / factor) elements.
std::vector<double> downsample_mean(const std::vector<double>& values,
                                    size_t factor);

// Finite-difference rate of change: out[i] = (values[i+1] - values[i]) /
// (timestamps[i+1] - timestamps[i]) for each adjacent pair, so `out` has one
// fewer element than the inputs. A pair whose timestamps are equal yields NaN
// for that slot rather than dividing by zero. Returns LengthMismatch (and
// clears `out`) when the two inputs differ in length; None otherwise, including
// for inputs shorter than two samples, which produce an empty `out`.
StatsError rate_of_change(const std::vector<double>& values,
                          const std::vector<uint64_t>& timestamps,
                          std::vector<double>& out);

// Faults per window_ns over the fault time span, i.e. fault_count / span_ns *
// window_ns where the span runs from the earliest to the latest fault
// timestamp. Returns 0 when there are no faults, and NaN when the span is zero
// (a single fault, or several sharing one timestamp) so the rate is undefined.
double fault_rate(const AnomalyEngine& engine, uint64_t window_ns);

// Mean gap, in nanoseconds, between consecutive faults in the engine's global
// fault ordering (the order faults() returns). With fewer than two faults there
// is no gap and the result is NaN. Gaps are summed as signed differences so an
// out-of-order timestamp does not wrap.
double mean_interfault_ns(const AnomalyEngine& engine);

// Counts of the engine's faults indexed by Severity (Info..Critical).
std::array<size_t, 4> severity_histogram(const AnomalyEngine& engine);

}  // namespace blackspan

#endif  // BLACKSPAN_STATS_H
