#ifndef BLACKSPAN_REPORT_H
#define BLACKSPAN_REPORT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/channel.h"
#include "blackspan/json_writer.h"
#include "blackspan/schema.h"
#include "blackspan/session.h"

namespace blackspan {

struct ReportModel {
  const std::vector<Session>* sessions = nullptr;
  const SchemaRegistry* schemas = nullptr;
  const ChannelStore* channels = nullptr;
  const AnomalyEngine* anomalies = nullptr;
};

enum ReportSection : uint16_t {
  kSectionSessions = 1u << 0,
  kSectionSchemas = 1u << 1,
  kSectionChannels = 1u << 2,
  kSectionAnomalies = 1u << 3,
};
constexpr uint16_t kSectionAll = 0xFFFFu;

enum class ReportFormat : uint8_t {
  Text = 0,
  Json = 1,
  Dot = 2,
};

const char* report_format_name(ReportFormat f);

std::string render_text(const ReportModel& model,
                        uint16_t section_mask = kSectionAll);
std::string render_json(const ReportModel& model,
                        uint16_t section_mask = kSectionAll);
std::string render_dot(const ReportModel& model,
                       uint16_t section_mask = kSectionAll);

std::string render(const ReportModel& model, ReportFormat format,
                   uint16_t section_mask = kSectionAll);

constexpr size_t kExportRequestSize = 3;

struct ExportRequest {
  ReportFormat format = ReportFormat::Text;
  uint16_t section_mask = 0;
};

enum class ExportError : uint8_t {
  None = 0,
  Truncated,
  BadFormat,
  TrailingBytes,
};

const char* export_error_str(ExportError e);

std::optional<ReportFormat> format_from_byte(uint8_t v);

ExportError decode_export_request(const uint8_t* payload, size_t len,
                                  ExportRequest& out);

std::string render(const ReportModel& model, const ExportRequest& request);

std::string format_number(double v);

std::string escape_dot(const std::string& s);

}

#endif
