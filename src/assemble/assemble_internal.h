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

void write_sample_block_header(uint16_t channel_id, uint16_t schema_version,
                               uint32_t base_ts_delta, uint16_t sample_count,
                               uint16_t flags, std::vector<uint8_t>& out);

enum class DamageMode : uint8_t {
  HeaderCrc,
  PayloadCrc,
  Magic,
  Version,
  Length,
  Truncate,
};

bool parse_damage_mode(const std::string& s, DamageMode& out);

bool apply_damage(std::vector<uint8_t>& bytes, size_t frame_start,
                  DamageMode mode, uint64_t amount);

}
}

#endif
