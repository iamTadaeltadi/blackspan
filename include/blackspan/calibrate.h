#ifndef BLACKSPAN_CALIBRATE_H
#define BLACKSPAN_CALIBRATE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "blackspan/channel.h"

// The calibrate subsystem turns the raw values a recorder stored into
// engineering units. A device writes a channel field as whatever its firmware
// had cheaply to hand — an ADC count, a fixed-point tick, a raw register — and
// leaves the conversion to the analysis tool. This subsystem carries that
// conversion: a calibration table maps a (channel, field) to an affine transform
// `engineering = raw * scale + offset` and a unit label, and applies it to the
// decoded sample columns so a report can show volts, amps, and degrees rather
// than counts.
//
// A table is authored in a small line-oriented text format, so a site's
// calibration can live beside its recordings and be version-controlled. Parsing
// reports every malformed line with its line number rather than stopping at the
// first. The subsystem reads the channel store and its own table; it mutates
// neither and never touches raw bytes. Nothing here throws.

namespace blackspan {

// One calibration rule: an affine transform and a unit label for a single field
// of a single channel. The identity rule (scale 1, offset 0, empty unit) leaves
// a value unchanged.
struct CalibrationRule {
  uint16_t channel_id = 0;
  size_t field_index = 0;
  double scale = 1.0;
  double offset = 0.0;
  std::string unit;

  // Applies the transform to a raw value.
  double apply(double raw) const { return raw * scale + offset; }
};

// Why a calibration line failed to parse.
enum class CalibrationError : uint8_t {
  None = 0,
  UnknownDirective,   // the line did not begin with "channel"
  MissingField,       // a required token (field/scale/offset/unit) was absent
  BadNumber,          // a numeric token did not parse
  BadFieldIndex,      // the field index was not a non-negative integer
  DuplicateRule,      // a (channel, field) pair was defined twice
};

// Human-readable form of a CalibrationError. Never null.
const char* calibration_error_str(CalibrationError e);

// One rejected calibration line.
struct CalibrationDiagnostic {
  size_t line = 0;
  CalibrationError error = CalibrationError::None;
  std::string detail;
};

// A table of calibration rules keyed by (channel, field). Missing entries mean
// "no calibration", i.e. the identity transform with no unit.
class CalibrationTable {
 public:
  // Installs a rule, replacing any existing rule for the same (channel, field).
  void set(const CalibrationRule& rule);

  // The rule for a (channel, field), or nullptr when none is defined.
  const CalibrationRule* get(uint16_t channel_id, size_t field_index) const;

  // The number of rules in the table.
  size_t size() const { return rules_.size(); }

  // All rules, ordered by (channel_id, field_index).
  std::vector<CalibrationRule> rules() const;

  // Applies the table to a raw value for a (channel, field): the rule's
  // transform when one exists, otherwise the value unchanged.
  double apply(uint16_t channel_id, size_t field_index, double raw) const;

  // Reads a channel field's raw column through the store and returns it in
  // engineering units. Samples the store could not read are dropped exactly as
  // ChannelStore::column_as_doubles drops them. The unit label, if any, is
  // written to *unit when non-null.
  std::vector<double> apply_column(const ChannelStore& store,
                                   uint16_t channel_id, size_t field_index,
                                   std::string* unit = nullptr) const;

  // Serializes the table back to the text format, one rule per line, ordered by
  // (channel, field). Parsing the result reproduces the table.
  std::string to_text() const;

 private:
  // key = (channel_id << 32) | field_index.
  std::map<uint64_t, CalibrationRule> rules_;
};

// The outcome of parsing a calibration table: the table and any diagnostics. The
// table holds every rule from a well-formed line even when other lines failed,
// so a caller can choose to proceed with a partial table or to treat any
// diagnostic as fatal.
struct CalibrationParse {
  CalibrationTable table;
  std::vector<CalibrationDiagnostic> errors;

  bool ok() const { return errors.empty(); }
};

// Parses a calibration table from text. Each non-blank, non-comment line has the
// form:
//   channel <id> field <index> scale <s> offset <o> [unit <label>]
// where <id> and <index> accept decimal or 0x-hex, <s> and <o> are reals, and
// <label> is a single whitespace-free token. The field/scale/offset/unit
// keywords may appear in any order; `scale` and `offset` may each be omitted and
// default to 1 and 0. A '#' begins a comment.
CalibrationParse parse_calibration(const std::string& text);

}  // namespace blackspan

#endif  // BLACKSPAN_CALIBRATE_H
