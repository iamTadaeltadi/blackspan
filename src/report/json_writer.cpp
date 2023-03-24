#include "blackspan/json_writer.h"

#include <cmath>
#include <cstdio>

namespace blackspan {

std::string escape_json(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(c));
          out += b;
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  return out;
}

void JsonWriter::before_value() {

  if (pending_key_) {
    pending_key_ = false;
    return;
  }
  if (!stack_.empty()) {
    Level& l = stack_.back();
    if (l.count > 0) out_ += ',';
    ++l.count;
  }
}

void JsonWriter::begin_object() {
  before_value();
  out_ += '{';
  stack_.push_back({'o', 0});
}

void JsonWriter::end_object() {
  out_ += '}';
  if (!stack_.empty()) stack_.pop_back();
}

void JsonWriter::begin_array() {
  before_value();
  out_ += '[';
  stack_.push_back({'a', 0});
}

void JsonWriter::end_array() {
  out_ += ']';
  if (!stack_.empty()) stack_.pop_back();
}

void JsonWriter::key(const std::string& k) {
  if (!stack_.empty()) {
    Level& l = stack_.back();
    if (l.count > 0) out_ += ',';
    ++l.count;
  }
  out_ += '"';
  out_ += escape_json(k);
  out_ += '"';
  out_ += ':';
  pending_key_ = true;
}

void JsonWriter::string_value(const std::string& v) {
  before_value();
  out_ += '"';
  out_ += escape_json(v);
  out_ += '"';
}

void JsonWriter::int_value(long long v) {
  before_value();
  out_ += std::to_string(v);
}

void JsonWriter::uint_value(unsigned long long v) {
  before_value();
  out_ += std::to_string(v);
}

void JsonWriter::double_value(double v) {
  before_value();

  if (!std::isfinite(v)) {
    out_ += "null";
    return;
  }
  char b[32];
  std::snprintf(b, sizeof(b), "%.6g", v);

  for (char* p = b; *p; ++p) {
    if (*p == ',') *p = '.';
  }
  out_ += b;
}

void JsonWriter::bool_value(bool v) {
  before_value();
  out_ += v ? "true" : "false";
}

void JsonWriter::null_value() {
  before_value();
  out_ += "null";
}

}
