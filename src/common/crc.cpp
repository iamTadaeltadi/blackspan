#include "blackspan/crc.h"

#include <array>

namespace blackspan {
namespace {

std::array<uint16_t, 256> make_crc16_table() {
  std::array<uint16_t, 256> table{};
  for (int i = 0; i < 256; ++i) {
    uint16_t c = static_cast<uint16_t>(i << 8);
    for (int b = 0; b < 8; ++b) {
      c = (c & 0x8000) ? static_cast<uint16_t>((c << 1) ^ 0x1021)
                       : static_cast<uint16_t>(c << 1);
    }
    table[i] = c;
  }
  return table;
}

std::array<uint32_t, 256> make_crc32_table() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int b = 0; b < 8; ++b) {
      c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

}

uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
  static const std::array<uint16_t, 256> table = make_crc16_table();
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    const uint8_t idx = static_cast<uint8_t>((crc >> 8) ^ data[i]);
    crc = static_cast<uint16_t>((crc << 8) ^ table[idx]);
  }
  return crc;
}

uint32_t crc32(const uint8_t* data, size_t len) {
  static const std::array<uint32_t, 256> table = make_crc32_table();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    const uint8_t idx = static_cast<uint8_t>(crc ^ data[i]);
    crc = table[idx] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

}
