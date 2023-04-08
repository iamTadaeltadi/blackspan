#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "blackspan/sketch.h"
#include "blackspan/stats_advanced.h"

namespace blackspan {
namespace {

uint64_t splitmix64(uint64_t& state) {
  state += 0x9e3779b97f4a7c15ULL;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

uint64_t mix_key(uint64_t key, uint64_t salt) {
  uint64_t z = key ^ salt;
  z += 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

double clamp_unit(double q) {
  if (std::isnan(q)) return std::numeric_limits<double>::quiet_NaN();
  if (q < 0.0) return 0.0;
  if (q > 1.0) return 1.0;
  return q;
}

double interp_sorted(const std::vector<double>& sorted, double q) {
  const size_t n = sorted.size();
  if (n == 0) return std::numeric_limits<double>::quiet_NaN();
  const double qc = clamp_unit(q);
  if (std::isnan(qc)) return std::numeric_limits<double>::quiet_NaN();
  if (n == 1) return sorted[0];
  const double pos = qc * static_cast<double>(n - 1);
  const double floor_pos = std::floor(pos);
  const size_t lo = static_cast<size_t>(floor_pos);
  const double frac = pos - floor_pos;
  if (lo + 1 >= n) return sorted[n - 1];
  return sorted[lo] + frac * (sorted[lo + 1] - sorted[lo]);
}

}

ReservoirSampler::ReservoirSampler(size_t capacity, uint64_t seed)
    : capacity_(capacity), state_(seed), seen_(0) {
  if (capacity_ > 0) reservoir_.reserve(capacity_);
}

uint64_t ReservoirSampler::next_random() { return splitmix64(state_); }

void ReservoirSampler::push(double v) {
  if (capacity_ == 0) {
    ++seen_;
    return;
  }
  if (reservoir_.size() < capacity_) {
    reservoir_.push_back(v);
  } else {
    const uint64_t span = static_cast<uint64_t>(seen_) + 1;
    const uint64_t j = next_random() % span;
    if (j < static_cast<uint64_t>(capacity_)) {
      reservoir_[static_cast<size_t>(j)] = v;
    }
  }
  ++seen_;
}

double ReservoirSampler::approx_quantile(double q) const {
  if (reservoir_.empty()) return std::numeric_limits<double>::quiet_NaN();
  std::vector<double> sorted = reservoir_;
  std::sort(sorted.begin(), sorted.end());
  return interp_sorted(sorted, q);
}

TDigestLite::TDigestLite(size_t max_centroids)
    : cap_(max_centroids == 0 ? 1 : max_centroids),
      count_(0),
      min_(std::numeric_limits<double>::infinity()),
      max_(-std::numeric_limits<double>::infinity()),
      total_weight_(0.0) {}

void TDigestLite::compress() const {
  while (centroids_.size() > cap_) {
    size_t best = 0;
    double best_gap = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i + 1 < centroids_.size(); ++i) {
      const double gap = centroids_[i + 1].mean - centroids_[i].mean;
      if (gap < best_gap) {
        best_gap = gap;
        best = i;
      }
    }
    Centroid& a = centroids_[best];
    const Centroid& b = centroids_[best + 1];
    const double w = a.weight + b.weight;
    a.mean = (a.mean * a.weight + b.mean * b.weight) / w;
    a.weight = w;
    centroids_.erase(centroids_.begin() + static_cast<std::ptrdiff_t>(best + 1));
  }
}

void TDigestLite::flush() const {
  if (buffer_.empty()) return;
  for (double v : buffer_) centroids_.push_back(Centroid{v, 1.0});
  buffer_.clear();
  std::sort(centroids_.begin(), centroids_.end(),
            [](const Centroid& a, const Centroid& b) { return a.mean < b.mean; });
  compress();
}

void TDigestLite::push(double v) {
  if (std::isnan(v)) return;
  if (v < min_) min_ = v;
  if (v > max_) max_ = v;
  buffer_.push_back(v);
  ++count_;
  total_weight_ += 1.0;
  const size_t batch = cap_ * 4 + 64;
  if (buffer_.size() >= batch) flush();
}

double TDigestLite::quantile(double q) const {
  flush();
  if (centroids_.empty()) return std::numeric_limits<double>::quiet_NaN();
  const double qc = clamp_unit(q);
  if (std::isnan(qc)) return std::numeric_limits<double>::quiet_NaN();
  const size_t n = centroids_.size();
  if (n == 1) return centroids_[0].mean;

  std::vector<double> rank(n, 0.0);
  double cum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    rank[i] = cum + centroids_[i].weight * 0.5;
    cum += centroids_[i].weight;
  }
  const double target = qc * total_weight_;

  if (target <= rank[0]) {
    const double span = rank[0];
    if (span <= 0.0) return centroids_[0].mean;
    const double t = target / span;
    return min_ + t * (centroids_[0].mean - min_);
  }
  if (target >= rank[n - 1]) {
    const double span = total_weight_ - rank[n - 1];
    if (span <= 0.0) return centroids_[n - 1].mean;
    const double t = (target - rank[n - 1]) / span;
    return centroids_[n - 1].mean + t * (max_ - centroids_[n - 1].mean);
  }
  for (size_t i = 0; i + 1 < n; ++i) {
    if (target <= rank[i + 1]) {
      const double span = rank[i + 1] - rank[i];
      if (span <= 0.0) return centroids_[i + 1].mean;
      const double t = (target - rank[i]) / span;
      return centroids_[i].mean + t * (centroids_[i + 1].mean - centroids_[i].mean);
    }
  }
  return centroids_[n - 1].mean;
}

double TDigestLite::cdf(double x) const {
  flush();
  if (centroids_.empty()) return 0.0;
  if (total_weight_ <= 0.0) return 0.0;
  if (std::isnan(x)) return std::numeric_limits<double>::quiet_NaN();

  const size_t n = centroids_.size();
  std::vector<double> vs;
  std::vector<double> rs;
  vs.reserve(n + 2);
  rs.reserve(n + 2);
  vs.push_back(min_);
  rs.push_back(0.0);
  double cum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    vs.push_back(centroids_[i].mean);
    rs.push_back(cum + centroids_[i].weight * 0.5);
    cum += centroids_[i].weight;
  }
  vs.push_back(max_);
  rs.push_back(total_weight_);

  if (x <= vs.front()) return 0.0;
  if (x >= vs.back()) return 1.0;
  for (size_t i = 0; i + 1 < vs.size(); ++i) {
    if (x <= vs[i + 1]) {
      const double vspan = vs[i + 1] - vs[i];
      if (vspan <= 0.0) return rs[i + 1] / total_weight_;
      const double t = (x - vs[i]) / vspan;
      return (rs[i] + t * (rs[i + 1] - rs[i])) / total_weight_;
    }
  }
  return 1.0;
}

void TDigestLite::merge(const TDigestLite& other) {
  other.flush();
  if (other.count_ == 0) return;
  flush();
  for (const Centroid& c : other.centroids_) centroids_.push_back(c);
  count_ += other.count_;
  total_weight_ += other.total_weight_;
  if (other.min_ < min_) min_ = other.min_;
  if (other.max_ > max_) max_ = other.max_;
  std::sort(centroids_.begin(), centroids_.end(),
            [](const Centroid& a, const Centroid& b) { return a.mean < b.mean; });
  compress();
}

double Moments::variance() const {
  if (n == 0) return 0.0;
  return m2 / static_cast<double>(n);
}

double Moments::stddev() const {
  const double v = variance();
  if (v <= 0.0) return 0.0;
  return std::sqrt(v);
}

Moments streaming_moments(const std::vector<double>& xs) {
  Moments m;
  for (double x : xs) {
    ++m.n;
    const double delta = x - m.mean;
    m.mean += delta / static_cast<double>(m.n);
    const double delta2 = x - m.mean;
    m.m2 += delta * delta2;
  }
  return m;
}

Moments combine_moments(const Moments& a, const Moments& b) {
  if (a.n == 0) return b;
  if (b.n == 0) return a;
  Moments r;
  r.n = a.n + b.n;
  const double na = static_cast<double>(a.n);
  const double nb = static_cast<double>(b.n);
  const double nt = static_cast<double>(r.n);
  const double delta = b.mean - a.mean;
  r.mean = a.mean + delta * (nb / nt);
  r.m2 = a.m2 + b.m2 + delta * delta * (na * nb / nt);
  return r;
}

CountMinSketch::CountMinSketch(size_t width, size_t depth, uint64_t seed)
    : width_(width), depth_(depth), total_(0) {
  uint64_t state = seed;
  row_seeds_.reserve(depth_);
  for (size_t r = 0; r < depth_; ++r) row_seeds_.push_back(splitmix64(state));
  if (width_ > 0 && depth_ > 0) counters_.assign(width_ * depth_, 0);
}

size_t CountMinSketch::bucket(uint64_t key, size_t row) const {
  const uint64_t h = mix_key(key, row_seeds_[row]);
  return static_cast<size_t>(h % static_cast<uint64_t>(width_));
}

void CountMinSketch::add(uint64_t key, uint64_t count) {
  total_ += count;
  if (width_ == 0 || depth_ == 0) return;
  for (size_t r = 0; r < depth_; ++r) {
    counters_[r * width_ + bucket(key, r)] += count;
  }
}

uint64_t CountMinSketch::estimate(uint64_t key) const {
  if (width_ == 0 || depth_ == 0) return 0;
  uint64_t best = std::numeric_limits<uint64_t>::max();
  for (size_t r = 0; r < depth_; ++r) {
    const uint64_t c = counters_[r * width_ + bucket(key, r)];
    if (c < best) best = c;
  }
  return best;
}

bool CountMinSketch::merge(const CountMinSketch& other) {
  if (width_ != other.width_ || depth_ != other.depth_) return false;
  if (row_seeds_ != other.row_seeds_) return false;
  for (size_t i = 0; i < counters_.size(); ++i) counters_[i] += other.counters_[i];
  total_ += other.total_;
  return true;
}

std::vector<FieldSketch> sketch_channels(const ChannelStore& store,
                                         size_t reservoir_capacity,
                                         uint64_t seed) {
  std::vector<FieldSketch> out;
  const std::vector<uint16_t> ids = store.channels();
  for (const uint16_t id : ids) {
    const ChannelSeries* series = store.series(id);
    if (series == nullptr) continue;
    const size_t field_count = series->schema().fields.size();
    for (size_t field = 0; field < field_count; ++field) {
      const std::vector<double> values = channel_column(store, id, field);

      FieldSketch fs;
      fs.channel_id = id;
      fs.field_index = field;
      fs.samples = values.size();

      const Moments m = streaming_moments(values);
      fs.mean = values.empty() ? 0.0 : m.mean;
      fs.stddev = m.stddev();

      if (values.empty()) {
        out.push_back(fs);
        continue;
      }

      if (reservoir_capacity == 0 || values.size() <= reservoir_capacity) {
        std::vector<double> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        fs.p50 = interp_sorted(sorted, 0.50);
        fs.p90 = interp_sorted(sorted, 0.90);
        fs.p99 = interp_sorted(sorted, 0.99);
      } else {
        ReservoirSampler rs(reservoir_capacity, seed ^ (static_cast<uint64_t>(id) << 32) ^ field);
        for (double v : values) rs.push(v);
        fs.p50 = rs.approx_quantile(0.50);
        fs.p90 = rs.approx_quantile(0.90);
        fs.p99 = rs.approx_quantile(0.99);
      }

      out.push_back(fs);
    }
  }
  return out;
}

std::string describe_sketch(const FieldSketch& s) {
  char line[192];
  std::snprintf(line, sizeof(line),
                "ch=%u field=%zu n=%zu p50=%.6g p90=%.6g p99=%.6g mean=%.6g "
                "sd=%.6g",
                static_cast<unsigned>(s.channel_id), s.field_index, s.samples,
                s.p50, s.p90, s.p99, s.mean, s.stddev);
  return std::string(line);
}

}
