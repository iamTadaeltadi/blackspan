#include "blackspan/report.h"

#include <cmath>
#include <cstdio>

#include "blackspan/byteio.h"
#include "report_internal.h"

namespace blackspan {

const char* report_format_name(ReportFormat f) {
  switch (f) {
    case ReportFormat::Text:
      return "text";
    case ReportFormat::Json:
      return "json";
    case ReportFormat::Dot:
      return "dot";
  }
  return "unknown";
}

const char* export_error_str(ExportError e) {
  switch (e) {
    case ExportError::None:
      return "ok";
    case ExportError::Truncated:
      return "truncated";
    case ExportError::BadFormat:
      return "bad-format";
    case ExportError::TrailingBytes:
      return "trailing-bytes";
  }
  return "unknown";
}

std::optional<ReportFormat> format_from_byte(uint8_t v) {
  switch (v) {
    case 0:
      return ReportFormat::Text;
    case 1:
      return ReportFormat::Json;
    case 2:
      return ReportFormat::Dot;
    default:
      return std::nullopt;
  }
}

ExportError decode_export_request(const uint8_t* payload, size_t len,
                                  ExportRequest& out) {
  out = ExportRequest{};
  if (len < kExportRequestSize) return ExportError::Truncated;
  ByteReader r(payload, len);
  const uint8_t fmt = r.u8();
  const uint16_t mask = r.u16();
  const std::optional<ReportFormat> f = format_from_byte(fmt);
  if (!f) return ExportError::BadFormat;
  if (len > kExportRequestSize) return ExportError::TrailingBytes;
  out.format = *f;
  out.section_mask = mask;
  return ExportError::None;
}

std::string render(const ReportModel& model, ReportFormat format,
                   uint16_t section_mask) {
  switch (format) {
    case ReportFormat::Text:
      return render_text(model, section_mask);
    case ReportFormat::Json:
      return render_json(model, section_mask);
    case ReportFormat::Dot:
      return render_dot(model, section_mask);
  }
  return std::string();
}

std::string render(const ReportModel& model, const ExportRequest& request) {
  const uint16_t mask =
      request.section_mask ? request.section_mask : kSectionAll;
  return render(model, request.format, mask);
}

std::string format_number(double v) {
  if (std::isnan(v)) return "nan";
  if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
  char b[32];
  std::snprintf(b, sizeof(b), "%.6g", v);
  // Guard against a locale that emits a comma as the decimal separator.
  for (char* p = b; *p; ++p) {
    if (*p == ',') *p = '.';
  }
  return std::string(b);
}

std::string escape_dot(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c < 0x20) {
      // Any other control character (carriage return, tab, ...) collapses to a
      // space so it can never break the quoted label or the line structure.
      out += ' ';
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

namespace report_detail {

std::string device_hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08x", v);
  return std::string(b);
}

ColumnStats column_stats(const std::vector<double>& xs) {
  ColumnStats s;
  if (xs.empty()) return s;
  s.present = true;
  s.count = xs.size();
  s.min = xs[0];
  s.max = xs[0];
  double sum = 0.0;
  for (double x : xs) {
    if (x < s.min) s.min = x;
    if (x > s.max) s.max = x;
    sum += x;
  }
  s.mean = sum / static_cast<double>(xs.size());
  return s;
}

bool want(uint16_t mask, uint16_t bit, const void* ptr) {
  return ptr != nullptr && (mask & bit) != 0;
}

}  // namespace report_detail

}  // namespace blackspan
