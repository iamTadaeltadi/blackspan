#ifndef BLACKSPAN_CALIBRATE_H
#define BLACKSPAN_CALIBRATE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "blackspan/channel.h"

namespace blackspan {

struct CalibrationRule {
  uint16_t channel_id = 0;
  size_t field_index = 0;
  double scale = 1.0;
  double offset = 0.0;
  std::string unit;

  double apply(double raw) const { return raw * scale + offset; }
};

enum class CalibrationError : uint8_t {
  None = 0,
  UnknownDirective,
  MissingField,
  BadNumber,
  BadFieldIndex,
  DuplicateRule,
};

const char* calibration_error_str(CalibrationError e);

struct CalibrationDiagnostic {
  size_t line = 0;
  CalibrationError error = CalibrationError::None;
  std::string detail;
};

class CalibrationTable {
 public:

  void set(const CalibrationRule& rule);

  const CalibrationRule* get(uint16_t channel_id, size_t field_index) const;

  size_t size() const { return rules_.size(); }

  std::vector<CalibrationRule> rules() const;

  double apply(uint16_t channel_id, size_t field_index, double raw) const;

  std::vector<double> apply_column(const ChannelStore& store,
                                   uint16_t channel_id, size_t field_index,
                                   std::string* unit = nullptr) const;

  std::string to_text() const;

 private:

  std::map<uint64_t, CalibrationRule> rules_;
};

struct CalibrationParse {
  CalibrationTable table;
  std::vector<CalibrationDiagnostic> errors;

  bool ok() const { return errors.empty(); }
};

CalibrationParse parse_calibration(const std::string& text);

}

#endif
