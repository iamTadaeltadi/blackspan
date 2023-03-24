#ifndef BLACKSPAN_SHA256_H
#define BLACKSPAN_SHA256_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace blackspan {

struct Sha256Digest {
  uint8_t bytes[32];

  std::string hex() const;
  bool operator==(const Sha256Digest& o) const;
  bool operator!=(const Sha256Digest& o) const { return !(*this == o); }
};

class Sha256 {
 public:
  Sha256();

  void update(const uint8_t* data, size_t len);
  Sha256Digest finish();

 private:
  void process_block(const uint8_t* block);

  uint32_t state_[8];
  uint64_t bit_len_;
  uint8_t buffer_[64];
  size_t buffer_len_;
};

Sha256Digest sha256(const uint8_t* data, size_t len);

}

#endif
