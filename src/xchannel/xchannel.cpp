#include <algorithm>
#include <cmath>
#include <cstdio>

#include "blackspan/aggregate.h"
#include "blackspan/stats_advanced.h"
#include "blackspan/xchannel.h"

namespace blackspan {
namespace {

double clamp_unit(double r) {
  if (r > 1.0) return 1.0;
  if (r < -1.0) return -1.0;
  return r;
}

double pearson_over(const std::vector<double>& xs,
                    const std::vector<double>& ys) {
  const size_t n = std::min(xs.size(), ys.size());
  if (n < 2) return 0.0;

  double mx = 0.0;
  double my = 0.0;
  for (size_t i = 0; i < n; ++i) {
    mx += xs[i];
    my += ys[i];
  }
  mx /= static_cast<double>(n);
  my /= static_cast<double>(n);

  double sxx = 0.0;
  double syy = 0.0;
  double sxy = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double dx = xs[i] - mx;
    const double dy = ys[i] - my;
    sxx += dx * dx;
    syy += dy * dy;
    sxy += dx * dy;
  }
  if (!(sxx > 0.0) || !(syy > 0.0)) return 0.0;
  return clamp_unit(sxy / std::sqrt(sxx * syy));
}

void split_rows(const JoinResult& jr, std::vector<double>& xs,
                std::vector<double>& ys) {
  xs.clear();
  ys.clear();
  xs.reserve(jr.rows.size());
  ys.reserve(jr.rows.size());
  for (const JoinedRow& row : jr.rows) {
    xs.push_back(row.left);
    ys.push_back(row.right);
  }
}

int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

}

PairStats pair_stats_aligned(const std::vector<double>& xs,
                             const std::vector<double>& ys) {
  PairStats s;
  const size_t n = std::min(xs.size(), ys.size());
  s.n = n;
  if (n == 0) return s;

  double mx = 0.0;
  double my = 0.0;
  for (size_t i = 0; i < n; ++i) {
    mx += xs[i];
    my += ys[i];
  }
  const double dn = static_cast<double>(n);
  mx /= dn;
  my /= dn;

  double sxx = 0.0;
  double syy = 0.0;
  double sxy = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double dx = xs[i] - mx;
    const double dy = ys[i] - my;
    sxx += dx * dx;
    syy += dy * dy;
    sxy += dx * dy;
  }

  s.covariance = sxy / dn;
  if (sxx > 0.0 && syy > 0.0) {
    s.correlation = clamp_unit(sxy / std::sqrt(sxx * syy));
  } else {
    s.correlation = 0.0;
  }

  LinearFit lf;
  linear_fit(xs, ys, lf);
  s.slope = lf.slope;
  s.intercept = lf.intercept;
  s.r2 = lf.r2;

  double sse = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double pred = s.slope * xs[i] + s.intercept;
    const double resid = ys[i] - pred;
    sse += resid * resid;
  }
  s.rmse = std::sqrt(sse / dn);
  return s;
}

PairStats pair_stats(const std::vector<std::pair<uint64_t, double>>& left,
                     const std::vector<std::pair<uint64_t, double>>& right,
                     JoinKind align, uint64_t tolerance_ns) {
  const JoinResult jr = join_series(left, right, align, tolerance_ns);
  std::vector<double> xs;
  std::vector<double> ys;
  split_rows(jr, xs, ys);
  return pair_stats_aligned(xs, ys);
}

double aligned_correlation(
    const std::vector<std::pair<uint64_t, double>>& left,
    const std::vector<std::pair<uint64_t, double>>& right, JoinKind align,
    uint64_t tolerance_ns) {
  return pair_stats(left, right, align, tolerance_ns).correlation;
}

std::string describe_ref(const ChannelFieldRef& ref) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "ch%u[%zu]",
                static_cast<unsigned>(ref.channel_id), ref.field_index);
  return std::string(buf);
}

std::vector<ChannelFieldRef> enumerate_fields(const ChannelStore& store) {
  std::vector<ChannelFieldRef> out;
  for (uint16_t ch : store.channels()) {
    const ChannelSeries* series = store.series(ch);
    if (series == nullptr) continue;
    const size_t field_count = series->schema().fields.size();
    for (size_t f = 0; f < field_count; ++f) {
      ChannelFieldRef ref;
      ref.channel_id = ch;
      ref.field_index = f;
      out.push_back(ref);
    }
  }
  return out;
}

std::vector<PairResult> correlation_matrix_with(const ChannelStore& store,
                                                JoinKind align,
                                                uint64_t tolerance_ns) {
  const std::vector<ChannelFieldRef> refs = enumerate_fields(store);

  std::vector<std::vector<std::pair<uint64_t, double>>> columns;
  columns.reserve(refs.size());
  for (const ChannelFieldRef& ref : refs) {
    columns.push_back(
        column_with_timestamps(store, ref.channel_id, ref.field_index));
  }

  std::vector<PairResult> out;
  if (refs.size() >= 2) out.reserve(refs.size() * (refs.size() - 1) / 2);
  for (size_t i = 0; i < refs.size(); ++i) {
    for (size_t j = i + 1; j < refs.size(); ++j) {
      PairResult pr;
      pr.a = refs[i];
      pr.b = refs[j];
      pr.stats = pair_stats(columns[i], columns[j], align, tolerance_ns);
      out.push_back(pr);
    }
  }
  return out;
}

std::vector<PairResult> correlation_matrix(const ChannelStore& store,
                                           uint64_t tolerance_ns) {
  return correlation_matrix_with(store, JoinKind::AsOfBackward, tolerance_ns);
}

std::vector<size_t> rank_pairs(const std::vector<PairResult>& pairs) {
  std::vector<size_t> idx(pairs.size());
  for (size_t i = 0; i < pairs.size(); ++i) idx[i] = i;
  std::stable_sort(idx.begin(), idx.end(), [&pairs](size_t a, size_t b) {
    return std::fabs(pairs[a].stats.correlation) >
           std::fabs(pairs[b].stats.correlation);
  });
  return idx;
}

std::vector<PairResult> top_correlated(const std::vector<PairResult>& pairs,
                                       size_t k) {
  const std::vector<size_t> idx = rank_pairs(pairs);
  const size_t m = std::min(k, idx.size());
  std::vector<PairResult> out;
  out.reserve(m);
  for (size_t i = 0; i < m; ++i) out.push_back(pairs[idx[i]]);
  return out;
}

std::vector<PairResult> filter_pairs(const std::vector<PairResult>& pairs,
                                     double min_abs_correlation) {
  std::vector<PairResult> out;
  for (const PairResult& p : pairs) {
    if (std::fabs(p.stats.correlation) >= min_abs_correlation) out.push_back(p);
  }
  return out;
}

std::string describe_pair(const PairResult& p) {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "%s ~ %s n=%zu r=%.4f cov=%.4g slope=%.4g intercept=%.4g "
                "r2=%.4f rmse=%.4g",
                describe_ref(p.a).c_str(), describe_ref(p.b).c_str(), p.stats.n,
                p.stats.correlation, p.stats.covariance, p.stats.slope,
                p.stats.intercept, p.stats.r2, p.stats.rmse);
  return std::string(buf);
}

std::string render_matrix(const std::vector<PairResult>& pairs) {
  if (pairs.empty()) return "0 pairs\n";
  std::string out;
  char head[48];
  std::snprintf(head, sizeof(head), "%zu pairs\n", pairs.size());
  out += head;
  for (const PairResult& p : pairs) {
    out += "  ";
    out += describe_pair(p);
    out += "\n";
  }
  return out;
}

std::vector<LagScore> lag_profile(
    const std::vector<std::pair<uint64_t, double>>& left,
    const std::vector<std::pair<uint64_t, double>>& right, JoinKind align,
    uint64_t tolerance_ns, int64_t max_lag) {
  std::vector<LagScore> out;
  if (max_lag < 0) return out;

  const JoinResult jr = join_series(left, right, align, tolerance_ns);
  std::vector<double> xs;
  std::vector<double> ys;
  split_rows(jr, xs, ys);

  const int64_t m = static_cast<int64_t>(xs.size());
  out.reserve(static_cast<size_t>(2 * max_lag + 1));
  for (int64_t lag = -max_lag; lag <= max_lag; ++lag) {
    const int64_t lo = std::max<int64_t>(0, -lag);
    const int64_t hi = std::min<int64_t>(m, m - lag);

    LagScore sc;
    sc.lag = lag;
    if (hi > lo) {
      std::vector<double> a;
      std::vector<double> b;
      a.reserve(static_cast<size_t>(hi - lo));
      b.reserve(static_cast<size_t>(hi - lo));
      for (int64_t i = lo; i < hi; ++i) {
        a.push_back(xs[static_cast<size_t>(i)]);
        b.push_back(ys[static_cast<size_t>(i + lag)]);
      }
      sc.overlap = a.size();
      sc.correlation = pearson_over(a, b);
    }
    out.push_back(sc);
  }
  return out;
}

LagScore best_lag(const std::vector<std::pair<uint64_t, double>>& left,
                  const std::vector<std::pair<uint64_t, double>>& right,
                  JoinKind align, uint64_t tolerance_ns, int64_t max_lag) {
  const std::vector<LagScore> profile =
      lag_profile(left, right, align, tolerance_ns, max_lag);
  LagScore best;
  if (profile.empty()) return best;

  best = profile.front();
  double best_abs = std::fabs(best.correlation);
  for (const LagScore& sc : profile) {
    const double sc_abs = std::fabs(sc.correlation);
    if (sc_abs > best_abs ||
        (sc_abs == best_abs && abs64(sc.lag) < abs64(best.lag))) {
      best = sc;
      best_abs = sc_abs;
    }
  }
  return best;
}

MatrixSummary summarize_matrix(const std::vector<PairResult>& pairs) {
  MatrixSummary s;
  s.pair_count = pairs.size();

  double sum_abs = 0.0;
  double max_abs = 0.0;
  size_t best = 0;
  bool has = false;
  for (size_t i = 0; i < pairs.size(); ++i) {
    if (pairs[i].stats.n < 2) continue;
    ++s.evaluated;
    const double a = std::fabs(pairs[i].stats.correlation);
    sum_abs += a;
    if (!has || a > max_abs) {
      max_abs = a;
      best = i;
      has = true;
    }
  }

  s.has_strongest = has;
  s.strongest_index = best;
  s.max_abs_correlation = has ? max_abs : 0.0;
  s.mean_abs_correlation =
      s.evaluated > 0 ? sum_abs / static_cast<double>(s.evaluated) : 0.0;
  return s;
}

std::string describe_matrix_summary(const MatrixSummary& s) {
  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "pairs=%zu evaluated=%zu max|r|=%.4f mean|r|=%.4f",
                s.pair_count, s.evaluated, s.max_abs_correlation,
                s.mean_abs_correlation);
  return std::string(buf);
}

}
