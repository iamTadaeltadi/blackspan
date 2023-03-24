#ifndef BLACKSPAN_CRC_H
#define BLACKSPAN_CRC_H

#include <cstddef>
#include <cstdint>

namespace blackspan {

uint16_t crc16_ccitt(const uint8_t* data, size_t len);

uint32_t crc32(const uint8_t* data, size_t len);

}

#endif
