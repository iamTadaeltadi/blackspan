#ifndef BLACKSPAN_SHA256_H
#define BLACKSPAN_SHA256_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace blackspan {

// A 256-bit digest with lowercase-hex rendering. Two digests compare equal iff
// every byte matches.
struct Sha256Digest {
  uint8_t bytes[32];

  std::string hex() const;
  bool operator==(const Sha256Digest& o) const;
  bool operator!=(const Sha256Digest& o) const { return !(*this == o); }
};

// Streaming SHA-256 (FIPS 180-4). Feed the message with update() in any
// chunking, then call finish() exactly once to obtain the digest. A finished
// object must not be updated or finished again; construct a fresh one instead.
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

// One-shot convenience wrapper around the streaming hasher.
Sha256Digest sha256(const uint8_t* data, size_t len);

}  // namespace blackspan

#endif  // BLACKSPAN_SHA256_H
