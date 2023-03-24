#include "blackspan/export.h"

#include <cstddef>
#include <cstdio>
#include <limits>

#include "blackspan/json_writer.h"

namespace blackspan {

namespace {

std::string fmt_g(double v) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.6g", v);

  for (char* p = b; *p; ++p) {
    if (*p == ',') *p = '.';
  }
  return std::string(b);
}

std::string device_hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08x", v);
  return std::string(b);
}

std::string hex_bytes(const std::vector<uint8_t>& bytes) {
  std::string out;
  out.reserve(bytes.size() * 2);
  char b[4];
  for (uint8_t byte : bytes) {
    std::snprintf(b, sizeof(b), "%02x", static_cast<unsigned>(byte));
    out += b;
  }
  return out;
}

void append_row(std::string& out, DelimFormat fmt,
                const std::vector<std::string>& fields) {
  const char d = delim_char(fmt);
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) out += d;
    out += quote_field(fields[i], fmt);
  }
  out += '\n';
}

}

const char* table_export_error_str(TableExportError e) {
  switch (e) {
    case TableExportError::None:
      return "ok";
    case TableExportError::UnknownChannel:
      return "unknown-channel";
    case TableExportError::BadField:
      return "bad-field";
  }
  return "unknown";
}

char delim_char(DelimFormat fmt) { return fmt == DelimFormat::Csv ? ',' : '\t'; }

std::string quote_field(const std::string& field, DelimFormat fmt) {
  if (fmt == DelimFormat::Csv) {
    const char d = delim_char(fmt);
    bool needs_quote = false;
    for (char c : field) {
      if (c == d || c == '"' || c == '\n' || c == '\r') {
        needs_quote = true;
        break;
      }
    }
    if (!needs_quote) return field;
    std::string out;
    out.reserve(field.size() + 2);
    out += '"';
    for (char c : field) {
      if (c == '"') out += '"';
      out += c;
    }
    out += '"';
    return out;
  }

  std::string out;
  out.reserve(field.size());
  for (char c : field) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

TableExportError export_channel_delimited(const ChannelStore& store,
                                          const SchemaRegistry& schemas,
                                          uint16_t channel_id, DelimFormat fmt,
                                          std::string& out) {
  out.clear();
  const ChannelSeries* series = store.series(channel_id);
  if (series == nullptr) return TableExportError::UnknownChannel;

  const ChannelSchema* schema = schemas.get(channel_id, series->schema_version());
  if (schema == nullptr) schema = &series->schema();
  if (schema->fields.empty()) return TableExportError::BadField;

  std::vector<std::string> header;
  header.reserve(schema->fields.size() + 2);
  header.emplace_back("index");
  header.emplace_back("timestamp");
  for (const SchemaField& f : schema->fields) header.push_back(f.name);
  append_row(out, fmt, header);

  const size_t field_count = schema->fields.size();
  std::vector<std::string> row;
  row.reserve(field_count + 2);

  size_t index = 0;
  for (size_t b = 0; b < series->block_count(); ++b) {
    const uint16_t count = series->block_sample_count(b);
    const uint64_t base_ts = series->block_base_ts(b);
    for (uint16_t j = 0; j < count; ++j, ++index) {
      row.clear();
      row.push_back(std::to_string(index));
      row.push_back(std::to_string(base_ts));
      const std::optional<SampleView> view = series->view(index);
      for (size_t f = 0; f < field_count; ++f) {
        std::optional<double> value;
        if (view.has_value()) value = view->get_as_double(f);
        row.push_back(value.has_value() ? fmt_g(*value) : "nan");
      }
      append_row(out, fmt, row);
    }
  }
  return TableExportError::None;
}

TableExportError export_faults_delimited(const AnomalyEngine& anomalies,
                                         DelimFormat fmt, std::string& out) {
  out.clear();
  append_row(out, fmt,
             {"device", "seq", "ts", "code", "severity", "subsystem",
              "detail"});
  for (const FaultEvent& f : anomalies.faults()) {
    append_row(out, fmt,
               {device_hex(f.device_id), std::to_string(f.session_seq),
                std::to_string(f.monotonic_ts), std::to_string(f.fault_code),
                severity_name(f.severity), std::to_string(f.subsystem_id),
                hex_bytes(f.detail)});
  }
  return TableExportError::None;
}

TableExportError export_transitions_delimited(const AnomalyEngine& anomalies,
                                              DelimFormat fmt,
                                              std::string& out) {
  out.clear();
  append_row(out, fmt,
             {"device", "seq", "ts", "from_state", "to_state", "cause",
              "continues_chain"});
  for (const TransitionEvent& t : anomalies.transitions()) {
    append_row(out, fmt,
               {device_hex(t.device_id), std::to_string(t.session_seq),
                std::to_string(t.monotonic_ts), std::to_string(t.from_state),
                std::to_string(t.to_state), std::to_string(t.cause),
                t.continues_chain ? "true" : "false"});
  }
  return TableExportError::None;
}

TableExportError export_sessions_delimited(const std::vector<Session>& sessions,
                                           DelimFormat fmt, std::string& out) {
  out.clear();
  append_row(out, fmt,
             {"device", "session_index", "device_class", "fw_major",
              "fw_minor", "fw_patch", "schema_epoch", "label", "begin_seq",
              "begin_ts", "end_seq", "end_ts", "closed", "end_reason",
              "duration_ns", "checkpoints", "reconnects", "markers",
              "seq_regressions", "ts_regressions"});
  for (const Session& s : sessions) {
    append_row(out, fmt,
               {device_hex(s.device_id), std::to_string(s.session_index),
                std::to_string(s.device_class), std::to_string(s.fw_major),
                std::to_string(s.fw_minor), std::to_string(s.fw_patch),
                std::to_string(s.schema_epoch), s.label,
                std::to_string(s.begin_seq), std::to_string(s.begin_ts),
                std::to_string(s.end_seq), std::to_string(s.end_ts),
                s.closed ? "true" : "false", std::to_string(s.end_reason),
                std::to_string(s.duration_ns()),
                std::to_string(s.checkpoints.size()),
                std::to_string(s.reconnects.size()),
                std::to_string(s.markers.size()),
                std::to_string(s.seq_regressions),
                std::to_string(s.ts_regressions)});
  }
  return TableExportError::None;
}

TableExportError export_channel_ndjson(const ChannelStore& store,
                                       const SchemaRegistry& schemas,
                                       uint16_t channel_id, std::string& out) {
  out.clear();
  const ChannelSeries* series = store.series(channel_id);
  if (series == nullptr) return TableExportError::UnknownChannel;
  const ChannelSchema* schema = schemas.get(channel_id, series->schema_version());
  if (schema == nullptr) schema = &series->schema();
  if (schema->fields.empty()) return TableExportError::BadField;

  const size_t field_count = schema->fields.size();
  size_t index = 0;
  for (size_t b = 0; b < series->block_count(); ++b) {
    const uint16_t count = series->block_sample_count(b);
    const uint64_t base_ts = series->block_base_ts(b);
    for (uint16_t j = 0; j < count; ++j, ++index) {
      const std::optional<SampleView> view = series->view(index);

      JsonWriter w;
      w.begin_object();
      w.key("index");
      w.uint_value(index);
      w.key("timestamp");
      w.uint_value(base_ts);
      for (size_t f = 0; f < field_count; ++f) {
        w.key(schema->fields[f].name);
        std::optional<double> value;
        if (view.has_value()) value = view->get_as_double(f);

        w.double_value(value.has_value() ? *value
                                         : std::numeric_limits<double>::quiet_NaN());
      }
      w.end_object();
      out += w.str();
      out += '\n';
    }
  }
  return TableExportError::None;
}

TableExportError export_faults_ndjson(const AnomalyEngine& anomalies,
                                      std::string& out) {
  out.clear();
  for (const FaultEvent& f : anomalies.faults()) {
    JsonWriter w;
    w.begin_object();
    w.key("device_id");
    w.uint_value(f.device_id);
    w.key("session_seq");
    w.uint_value(f.session_seq);
    w.key("monotonic_ts");
    w.uint_value(f.monotonic_ts);
    w.key("fault_code");
    w.uint_value(f.fault_code);
    w.key("severity");
    w.string_value(severity_name(f.severity));
    w.key("subsystem_id");
    w.uint_value(f.subsystem_id);
    w.key("detail");
    w.string_value(hex_bytes(f.detail));
    w.key("state_at_fault");
    if (f.state_at_fault.has_value()) {
      w.uint_value(f.state_at_fault.value());
    } else {
      w.null_value();
    }
    w.end_object();
    out += w.str();
    out += '\n';
  }
  return TableExportError::None;
}

}
