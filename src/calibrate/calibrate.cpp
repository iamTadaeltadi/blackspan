#include <cstdio>
#include <sstream>

#include "blackspan/calibrate.h"
#include "blackspan/textparse.h"

namespace blackspan {
namespace {

uint64_t make_key(uint16_t channel_id, size_t field_index) {
  return (static_cast<uint64_t>(channel_id) << 32) |
         (static_cast<uint32_t>(field_index));
}

}

const char* calibration_error_str(CalibrationError e) {
  switch (e) {
    case CalibrationError::None: return "none";
    case CalibrationError::UnknownDirective: return "unknown-directive";
    case CalibrationError::MissingField: return "missing-field";
    case CalibrationError::BadNumber: return "bad-number";
    case CalibrationError::BadFieldIndex: return "bad-field-index";
    case CalibrationError::DuplicateRule: return "duplicate-rule";
  }
  return "unknown";
}

void CalibrationTable::set(const CalibrationRule& rule) {
  rules_[make_key(rule.channel_id, rule.field_index)] = rule;
}

const CalibrationRule* CalibrationTable::get(uint16_t channel_id,
                                             size_t field_index) const {
  auto it = rules_.find(make_key(channel_id, field_index));
  if (it == rules_.end()) return nullptr;
  return &it->second;
}

std::vector<CalibrationRule> CalibrationTable::rules() const {
  std::vector<CalibrationRule> out;
  out.reserve(rules_.size());
  for (const auto& kv : rules_) out.push_back(kv.second);
  return out;
}

double CalibrationTable::apply(uint16_t channel_id, size_t field_index,
                               double raw) const {
  const CalibrationRule* rule = get(channel_id, field_index);
  return rule ? rule->apply(raw) : raw;
}

std::vector<double> CalibrationTable::apply_column(const ChannelStore& store,
                                                   uint16_t channel_id,
                                                   size_t field_index,
                                                   std::string* unit) const {
  std::vector<double> column = store.column_as_doubles(channel_id, field_index);
  const CalibrationRule* rule = get(channel_id, field_index);
  if (unit) *unit = rule ? rule->unit : std::string();
  if (rule) {
    for (double& v : column) v = rule->apply(v);
  }
  return column;
}

std::string CalibrationTable::to_text() const {
  std::string out;
  for (const auto& kv : rules_) {
    const CalibrationRule& r = kv.second;
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "channel %u field %zu scale %.10g offset %.10g",
                  r.channel_id, r.field_index, r.scale, r.offset);
    out += buf;
    if (!r.unit.empty()) {
      out += " unit ";
      out += r.unit;
    }
    out += "\n";
  }
  return out;
}

CalibrationParse parse_calibration(const std::string& text) {
  CalibrationParse result;

  for (const SourceLine& sl : source_lines(text)) {
    const size_t line_no = sl.number;
    std::istringstream iss(sl.text);
    std::string first;
    if (!(iss >> first)) continue;

    auto fail = [&](CalibrationError e, const std::string& detail) {
      CalibrationDiagnostic d;
      d.line = line_no;
      d.error = e;
      d.detail = detail;
      result.errors.push_back(std::move(d));
    };

    if (first != "channel") {
      fail(CalibrationError::UnknownDirective, first);
      continue;
    }

    std::string channel_tok;
    if (!(iss >> channel_tok)) {
      fail(CalibrationError::MissingField, "channel id");
      continue;
    }
    uint64_t channel_id = 0;
    if (!parse_uint_token(channel_tok, channel_id) || channel_id > 0xffff) {
      fail(CalibrationError::BadNumber, "channel=" + channel_tok);
      continue;
    }

    CalibrationRule rule;
    rule.channel_id = static_cast<uint16_t>(channel_id);
    bool have_field = false;
    bool bad = false;

    std::string kw;
    while (iss >> kw) {
      if (kw == "field") {
        std::string v;
        if (!(iss >> v)) {
          fail(CalibrationError::MissingField, "field");
          bad = true;
          break;
        }
        uint64_t idx = 0;
        if (!parse_uint_token(v, idx)) {
          fail(CalibrationError::BadFieldIndex, v);
          bad = true;
          break;
        }
        rule.field_index = static_cast<size_t>(idx);
        have_field = true;
      } else if (kw == "scale") {
        std::string v;
        if (!(iss >> v) || !parse_double_token(v, rule.scale)) {
          fail(CalibrationError::BadNumber, "scale");
          bad = true;
          break;
        }
      } else if (kw == "offset") {
        std::string v;
        if (!(iss >> v) || !parse_double_token(v, rule.offset)) {
          fail(CalibrationError::BadNumber, "offset");
          bad = true;
          break;
        }
      } else if (kw == "unit") {

        std::string v;
        if (!(iss >> v)) {
          fail(CalibrationError::MissingField, "unit");
          bad = true;
          break;
        }
        rule.unit = v;
      } else {
        fail(CalibrationError::MissingField, kw);
        bad = true;
        break;
      }
    }
    if (bad) continue;
    if (!have_field) {
      fail(CalibrationError::MissingField, "field");
      continue;
    }
    if (result.table.get(rule.channel_id, rule.field_index) != nullptr) {
      fail(CalibrationError::DuplicateRule,
           "channel " + std::to_string(rule.channel_id) + " field " +
               std::to_string(rule.field_index));
      continue;
    }
    result.table.set(rule);
  }

  return result;
}

}
