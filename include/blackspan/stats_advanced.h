#ifndef BLACKSPAN_STATS_ADVANCED_H
#define BLACKSPAN_STATS_ADVANCED_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "blackspan/channel.h"

// The stats_advanced subsystem is the deeper descriptive-statistics layer a
// triage report reaches for once the basic ColumnStats (count, min/max, mean,
// population stddev) in the stats subsystem are not enough: the shape of a
// distribution, its tails, its dispersion, and the relationship between two
// series. Everything here is a pure function of a numeric sample vector (or a
// pair of them); nothing mutates its inputs, allocates beyond its result, or
// throws. Degenerate inputs -- empty, a single element, a constant series, a
// zero-variance regressor -- have documented, non-crashing returns rather than
// undefined behaviour, so a caller never divides by zero or reads a stale value.
//
// The routines operate on plain std::vector<double> samples so they compose with
// anything that can produce a numeric column. The thin ChannelStore adapter at
// the end pulls a field column through the aggregate subsystem's block-walk and
// hands it to these functions, which is how the report layer runs them over a
// decoded recording.

namespace blackspan {

// Interpolation rule for percentile(). NearestRank returns an order statistic
// unchanged (the value at ordinal rank ceil(p/100 * N)); Linear interpolates
// between the two order statistics that bracket the fractional rank
// p/100 * (N - 1), which is the rule the basic stats layer uses.
enum class PercentileMethod : uint8_t { NearestRank = 0, Linear = 1 };

// The p-th percentile (p in [0, 100], clamped) of xs under method m, computed on
// a sorted local copy so xs is left untouched. An empty input, or a NaN p, yields
// NaN; a single-element input yields that element for either method.
double percentile(const std::vector<double>& xs, double p, PercentileMethod m);

// The median: percentile(xs, 50, Linear). Empty input yields NaN.
double median(const std::vector<double>& xs);

// The interquartile range p75 - p25 under Linear interpolation. Empty input
// yields NaN; a single-element or constant input yields 0.
double quantile_iqr(const std::vector<double>& xs);

// The variance of xs. With sample == true this is the unbiased estimator that
// divides the sum of squared deviations by N - 1; with sample == false it is the
// population variance that divides by N. Computed with a stable two-pass sum of
// squared deviations. An empty input, or a sample variance of a single element
// (N - 1 == 0), yields 0.
double variance(const std::vector<double>& xs, bool sample);

// The standard deviation: the square root of variance(xs, sample). Same
// degenerate returns as variance().
double stddev(const std::vector<double>& xs, bool sample);

// The population skewness (the third standardized moment). Returns 0 for fewer
// than three samples or a zero-variance (constant) series, where it is undefined.
// A right-tailed distribution gives a positive value, a left-tailed one negative.
double skewness(const std::vector<double>& xs);

// The population excess kurtosis (the fourth standardized moment minus 3, so a
// normal distribution is 0). Returns 0 for a degenerate input: fewer than two
// samples or a zero-variance series. A heavy-tailed distribution gives a positive
// value, a flat one negative.
double kurtosis_excess(const std::vector<double>& xs);

// A fixed-width histogram of `bins` equal-width buckets spanning [min, max] of
// the data. counts has exactly `bins` entries; width is (max - min) / bins.
// Bucket k covers the half-open range [min + k*width, min + (k+1)*width), except
// that a value exactly equal to max is counted in the last bucket. A constant
// series (max == min) reports a zero width and routes every value into bucket 0.
// bins == 0 yields an empty histogram; an empty input yields all-zero counts.
struct Histogram {
  std::vector<size_t> counts;
  double lo = 0.0;
  double hi = 0.0;
  double width = 0.0;
};
Histogram histogram(const std::vector<double>& xs, size_t bins);

// The exponentially weighted moving average with smoothing factor alpha in
// (0, 1] (clamped into that range): out[0] = x[0] and
// out[i] = alpha*x[i] + (1 - alpha)*out[i-1] for i > 0. The result has the same
// length as xs; an empty input yields an empty vector.
std::vector<double> ewma(const std::vector<double>& xs, double alpha);

// The ordinary-least-squares line y = slope*x + intercept fitted to the paired
// points (xs[i], ys[i]) over their common length, with r2 the coefficient of
// determination. A degenerate fit -- fewer than two paired points or a
// zero-variance regressor -- returns false and sets slope 0, intercept to the
// mean of the y values used, and r2 0; a non-degenerate fit returns true.
struct LinearFit {
  double slope = 0.0;
  double intercept = 0.0;
  double r2 = 0.0;
};
bool linear_fit(const std::vector<double>& xs, const std::vector<double>& ys,
                LinearFit& out);

// The median absolute deviation about the median, scaled by 1.4826 so that it is
// a consistent estimator of the standard deviation for normally distributed data.
// An empty input yields 0; a constant series yields 0.
double mad(const std::vector<double>& xs);

// The indices (ascending) of elements whose scaled distance from the median
// exceeds k, i.e. |xs[i] - median(xs)| / mad(xs) > k. A mad of 0 (constant
// series) or an empty input yields no outliers.
std::vector<size_t> mad_outliers(const std::vector<double>& xs, double k);

// The sample autocorrelation of xs at the given lag under the standard
// normalized estimator: the lag-k lagged-product sum over the total sum of
// squared deviations. A lag outside 1..N-1, or a zero-variance series, yields 0.
double autocorrelation(const std::vector<double>& xs, size_t lag);

// Pulls one field of one channel from the store as a plain value column, reading
// it through the aggregate subsystem's block-walk and dropping the timestamps.
// An unknown channel or an out-of-range field yields an empty vector, which the
// routines above map to their documented degenerate returns.
std::vector<double> channel_column(const ChannelStore& store, uint16_t channel_id,
                                   size_t field_index);

}  // namespace blackspan

#endif  // BLACKSPAN_STATS_ADVANCED_H
