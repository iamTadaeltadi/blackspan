#ifndef BLACKSPAN_EXPORT_H
#define BLACKSPAN_EXPORT_H

#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/channel.h"
#include "blackspan/schema.h"
#include "blackspan/session.h"

namespace blackspan {

enum class DelimFormat {
  Csv,
  Tsv,
};

enum class TableExportError : uint8_t {
  None = 0,
  UnknownChannel,
  BadField,
};

const char* table_export_error_str(TableExportError e);

char delim_char(DelimFormat fmt);

std::string quote_field(const std::string& field, DelimFormat fmt);

TableExportError export_channel_delimited(const ChannelStore& store,
                                          const SchemaRegistry& schemas,
                                          uint16_t channel_id, DelimFormat fmt,
                                          std::string& out);

TableExportError export_faults_delimited(const AnomalyEngine& anomalies,
                                         DelimFormat fmt, std::string& out);

TableExportError export_transitions_delimited(const AnomalyEngine& anomalies,
                                              DelimFormat fmt, std::string& out);

TableExportError export_sessions_delimited(const std::vector<Session>& sessions,
                                           DelimFormat fmt, std::string& out);

TableExportError export_channel_ndjson(const ChannelStore& store,
                                       const SchemaRegistry& schemas,
                                       uint16_t channel_id, std::string& out);

TableExportError export_faults_ndjson(const AnomalyEngine& anomalies,
                                      std::string& out);

}

#endif
