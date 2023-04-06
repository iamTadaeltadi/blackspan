#ifndef BLACKSPAN_FOLD_H
#define BLACKSPAN_FOLD_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

struct PeriodEstimate {
  size_t period = 0;
  double strength = 0.0;
  bool found = false;
};

PeriodEstimate estimate_period(const std::vector<double>& xs, size_t min_period,
                               size_t max_period);

struct PeriodScan {
  std::vector<size_t> periods;
  std::vector<double> autocorr;
  std::vector<double> score;
};

PeriodScan scan_periods(const std::vector<double>& xs, size_t min_period,
                        size_t max_period);

size_t fundamental_period(const std::vector<double>& xs, size_t period,
                          double tolerance);

struct FoldedProfile {
  size_t period = 0;
  std::vector<double> mean;
  std::vector<double> band;
  size_t cycles = 0;
};

FoldedProfile fold_series(const std::vector<double>& xs, size_t period);

double periodicity_score(const std::vector<double>& xs, size_t period);

double cycle_consistency(const std::vector<double>& xs, size_t period);

struct ResidualStats {
  double rms = 0.0;
  double max_abs = 0.0;
  size_t worst_index = 0;
};

ResidualStats fold_residual(const std::vector<double>& xs,
                            const FoldedProfile& profile);

double profile_amplitude(const FoldedProfile& profile);

size_t profile_peak_phase(const FoldedProfile& profile);

size_t profile_trough_phase(const FoldedProfile& profile);

std::vector<double> reconstruct(const FoldedProfile& profile, size_t count);

std::string render_profile(const FoldedProfile& profile);

struct FoldSummary {
  uint16_t channel_id = 0;
  size_t field_index = 0;
  size_t samples = 0;
  size_t period = 0;
  double strength = 0.0;
  double residual_rms = 0.0;
};

std::vector<FoldSummary> analyze_folds(const ChannelStore& store);

std::string describe_fold(const FoldSummary& f);

std::string render_folds(const std::vector<FoldSummary>& fs);

}

#endif
