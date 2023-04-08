#ifndef BLACKSPAN_SKETCH_H
#define BLACKSPAN_SKETCH_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

class ReservoirSampler {
 public:
  explicit ReservoirSampler(size_t capacity, uint64_t seed);

  void push(double v);

  const std::vector<double>& samples() const { return reservoir_; }

  size_t seen() const { return seen_; }

  double approx_quantile(double q) const;

 private:
  uint64_t next_random();

  size_t capacity_;
  uint64_t state_;
  size_t seen_;
  std::vector<double> reservoir_;
};

class TDigestLite {
 public:
  explicit TDigestLite(size_t max_centroids);

  void push(double v);

  double quantile(double q) const;

  double cdf(double x) const;

  void merge(const TDigestLite& other);

  size_t count() const { return count_; }

 private:
  struct Centroid {
    double mean;
    double weight;
  };

  void flush() const;

  void compress() const;

  size_t cap_;
  size_t count_;
  double min_;
  double max_;
  mutable std::vector<double> buffer_;
  mutable std::vector<Centroid> centroids_;
  mutable double total_weight_;
};

struct Moments {
  size_t n = 0;
  double mean = 0.0;
  double m2 = 0.0;

  double variance() const;
  double stddev() const;
};

Moments streaming_moments(const std::vector<double>& xs);

Moments combine_moments(const Moments& a, const Moments& b);

class CountMinSketch {
 public:
  CountMinSketch(size_t width, size_t depth, uint64_t seed);

  void add(uint64_t key, uint64_t count);

  uint64_t estimate(uint64_t key) const;

  bool merge(const CountMinSketch& other);

  uint64_t total() const { return total_; }

 private:
  size_t bucket(uint64_t key, size_t row) const;

  size_t width_;
  size_t depth_;
  uint64_t total_;
  std::vector<uint64_t> row_seeds_;
  std::vector<uint64_t> counters_;
};

struct FieldSketch {
  uint16_t channel_id = 0;
  size_t field_index = 0;
  size_t samples = 0;
  double p50 = 0.0;
  double p90 = 0.0;
  double p99 = 0.0;
  double mean = 0.0;
  double stddev = 0.0;
};

std::vector<FieldSketch> sketch_channels(const ChannelStore& store,
                                         size_t reservoir_capacity,
                                         uint64_t seed);

std::string describe_sketch(const FieldSketch& s);

}

#endif
