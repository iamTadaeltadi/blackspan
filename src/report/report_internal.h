#ifndef BLACKSPAN_SRC_REPORT_INTERNAL_H
#define BLACKSPAN_SRC_REPORT_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/report.h"

namespace blackspan {
namespace report_detail {

std::string device_hex(uint32_t v);

struct ColumnStats {
  bool present = false;
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  size_t count = 0;
};

ColumnStats column_stats(const std::vector<double>& xs);

bool want(uint16_t mask, uint16_t bit, const void* ptr);

}
}

#endif
