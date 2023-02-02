#ifndef BLACKSPAN_CRC_H
#define BLACKSPAN_CRC_H

#include <cstddef>
#include <cstdint>

namespace blackspan {

// CRC-16/CCITT-FALSE: polynomial 0x1021, init 0xFFFF, no input/output
// reflection, no final xor. Used for recorder frame headers.
uint16_t crc16_ccitt(const uint8_t* data, size_t len);

// CRC-32 (ISO-HDLC / zlib): reflected polynomial 0xEDB88320, init and final
// xor 0xFFFFFFFF. Used for recorder frame payloads.
uint32_t crc32(const uint8_t* data, size_t len);

}  // namespace blackspan

#endif  // BLACKSPAN_CRC_H
