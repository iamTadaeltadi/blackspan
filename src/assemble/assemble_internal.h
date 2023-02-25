#ifndef BLACKSPAN_SRC_ASSEMBLE_INTERNAL_H
#define BLACKSPAN_SRC_ASSEMBLE_INTERNAL_H

#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/assemble.h"
#include "blackspan/derive.h"
#include "blackspan/report.h"
#include "blackspan/session.h"

// Payload builders shared by the script parser. Each writes the little-endian
// grammar its record type owns, exactly as the matching decoder reads it. They
// are internal to the assemble subsystem: the public entry point is
// assemble_script().

namespace blackspan {
namespace assemble_detail {

void write_session_begin(const SessionBegin& r, std::vector<uint8_t>& out);
void write_session_end(const SessionEnd& r, std::vector<uint8_t>& out);
void write_checkpoint(const Checkpoint& r, std::vector<uint8_t>& out);
void write_reconnect(const Reconnect& r, std::vector<uint8_t>& out);
void write_marker(const Marker& r, std::vector<uint8_t>& out);
void write_state_transition(const StateTransition& r,
                            std::vector<uint8_t>& out);
void write_config_change(const ConfigChange& r, std::vector<uint8_t>& out);
void write_fault(const Fault& r, std::vector<uint8_t>& out);
void write_derived_def(const DerivedDefRecord& r, std::vector<uint8_t>& out);
void write_export_request(const ExportRequest& r, std::vector<uint8_t>& out);

// Writes the fixed sample-block header that precedes packed rows.
void write_sample_block_header(uint16_t channel_id, uint16_t schema_version,
                               uint32_t base_ts_delta, uint16_t sample_count,
                               uint16_t flags, std::vector<uint8_t>& out);

// Modes accepted by the damage directive.
// The Version and Length modes edit a header field and then reseal the header
// CRC, so the decoder reaches the check the mode is meant to exercise instead
// of stopping at a CRC mismatch. The remaining modes leave the frame
// inconsistent on purpose.
enum class DamageMode : uint8_t {
  HeaderCrc,   // corrupt the header CRC field of the last frame
  PayloadCrc,  // corrupt the payload CRC trailer of the last frame
  Magic,       // corrupt the frame magic
  Version,     // declare an unsupported format version
  Length,      // overstate the declared payload length
  Truncate,    // cut bytes from the end of the last frame
};

// Parses a damage mode name, or returns false for an unknown one.
bool parse_damage_mode(const std::string& s, DamageMode& out);

// Applies a damage mode to the frame that occupies bytes[frame_start, end).
// `amount` is the byte count for Truncate and is otherwise ignored. Returns
// false when the frame is too short for the requested damage.
bool apply_damage(std::vector<uint8_t>& bytes, size_t frame_start,
                  DamageMode mode, uint64_t amount);

}  // namespace assemble_detail
}  // namespace blackspan

#endif  // BLACKSPAN_SRC_ASSEMBLE_INTERNAL_H
