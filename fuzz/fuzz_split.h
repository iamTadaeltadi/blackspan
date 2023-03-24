#ifndef BLACKSPAN_FUZZ_SPLIT_H
#define BLACKSPAN_FUZZ_SPLIT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SplitInput {
  std::vector<uint8_t> recording;
  std::string text;
};

inline SplitInput split_input(const uint8_t* data, size_t size) {
  SplitInput out;
  if (size < 2) return out;
  size_t n = static_cast<size_t>(data[0]) | (static_cast<size_t>(data[1]) << 8);
  size_t avail = size - 2;
  if (n > avail) n = avail;
  out.recording.assign(data + 2, data + 2 + n);
  out.text.assign(reinterpret_cast<const char*>(data + 2 + n), avail - n);
  return out;
}

#endif
