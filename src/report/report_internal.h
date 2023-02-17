#ifndef BLACKSPAN_SRC_REPORT_INTERNAL_H
#define BLACKSPAN_SRC_REPORT_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/report.h"

// Helpers shared by the three renderers. These are internal to the report
// subsystem (not part of the public header) so the public surface stays the
// model, the renderers, and the export-request grammar.

namespace blackspan {
namespace report_detail {

// 8-hex-digit device id, matching the "%08x" form used by the domain
// describe_* helpers so device ids read identically everywhere.
std::string device_hex(uint32_t v);

// min / max / mean over a column of doubles. `present` is false for an empty
// column, in which case the numeric fields are left at zero.
struct ColumnStats {
  bool present = false;
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  size_t count = 0;
};

ColumnStats column_stats(const std::vector<double>& xs);

// True when `bit` is set in `mask` and `ptr` (the section's backing view) is
// non-null: the two conditions under which a section is rendered.
bool want(uint16_t mask, uint16_t bit, const void* ptr);

}  // namespace report_detail
}  // namespace blackspan

#endif  // BLACKSPAN_SRC_REPORT_INTERNAL_H
