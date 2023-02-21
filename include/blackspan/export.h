#ifndef BLACKSPAN_EXPORT_H
#define BLACKSPAN_EXPORT_H

#include <cstdint>
#include <string>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/channel.h"
#include "blackspan/schema.h"
#include "blackspan/session.h"

// The export subsystem is a leaf consumer of the decode pipeline, next to the
// report subsystem: it renders an already-reconstructed model to the tabular
// formats external tools expect. Delimited output (CSV/TSV) is a header row plus
// one row per record; NDJSON is one self-contained JSON object per line. Every
// exporter is a pure function of its input, mutates nothing it is handed, and
// produces byte-deterministic output for a given model, so exporting the same
// model twice yields identical bytes. Field text is always quoted or escaped so
// that a delimiter, quote, or line break inside a value can never break the row
// or column structure. Nothing here throws: a bad request yields a specific
// TableExportError value and an empty output string.

namespace blackspan {

// The two delimiter-separated tabular formats this subsystem emits. Csv uses a
// comma and RFC-4180-style double-quote quoting; Tsv uses a tab and C-style
// backslash escaping of the characters that would otherwise break a cell.
enum class DelimFormat {
  Csv,
  Tsv,
};

// Error codes for the table exporters. Errors are values; nothing here throws.
// (Named TableExportError to avoid colliding with the report subsystem's
// unrelated ExportError, which describes export-request decoding.)
enum class TableExportError : uint8_t {
  None = 0,
  UnknownChannel,  // no series has been ingested for the requested channel
  BadField,        // the channel resolved to a schema with no readable fields
};

// Human-readable form of a TableExportError. Never null.
const char* table_export_error_str(TableExportError e);

// The delimiter character for a format: ',' for Csv, '\t' for Tsv.
char delim_char(DelimFormat fmt);

// Quotes or escapes a single field so it is safe to place between delimiters.
//   Csv: if the field contains the delimiter, a double quote, a newline, or a
//        carriage return it is wrapped in double quotes and each internal quote
//        is doubled; otherwise it passes through unchanged.
//   Tsv: backslash, tab, newline, and carriage return become the two-character
//        escapes \\, \t, \n, and \r, so the result contains none of them and is
//        losslessly reversible.
// The result never contains a raw delimiter or line break, so it can never
// disturb the surrounding row or column structure.
std::string quote_field(const std::string& field, DelimFormat fmt);

// Renders one channel's retained samples as a delimited table into out (which is
// cleared first). The header is "index,timestamp,<field0>,<field1>,..." and each
// subsequent row carries the sample's global index, its block base timestamp,
// and every field widened to double and formatted with "%.6g". Returns
// UnknownChannel if the channel has no series, BadField if its schema exposes no
// fields, else None.
TableExportError export_channel_delimited(const ChannelStore& store,
                                          const SchemaRegistry& schemas,
                                          uint16_t channel_id, DelimFormat fmt,
                                          std::string& out);

// Renders every recorded fault as a delimited table into out (cleared first).
// Columns: device,seq,ts,code,severity,subsystem,detail. The device is an
// 8-hex-digit id, detail is the lowercase hex of the fault's detail bytes, and
// severity is its mnemonic name. Always returns None.
TableExportError export_faults_delimited(const AnomalyEngine& anomalies,
                                         DelimFormat fmt, std::string& out);

// Renders every recorded state transition as a delimited table into out
// (cleared first). Columns:
// device,seq,ts,from_state,to_state,cause,continues_chain. Always returns None.
TableExportError export_transitions_delimited(const AnomalyEngine& anomalies,
                                              DelimFormat fmt, std::string& out);

// Renders a list of reconstructed sessions as a delimited table into out
// (cleared first). Columns cover the device and firmware identity, the begin and
// end frame positions, the close state, and the per-session event counts.
// Always returns None.
TableExportError export_sessions_delimited(const std::vector<Session>& sessions,
                                           DelimFormat fmt, std::string& out);

// Renders one channel's retained samples as NDJSON into out (cleared first): one
// compact JSON object per line, each with "index", "timestamp", and one member
// per field (widened to double). Returns UnknownChannel / BadField as
// export_channel_delimited does, else None.
TableExportError export_channel_ndjson(const ChannelStore& store,
                                       const SchemaRegistry& schemas,
                                       uint16_t channel_id, std::string& out);

// Renders every recorded fault as NDJSON into out (cleared first): one compact
// JSON object per line carrying the fault's frame position, code, severity name,
// subsystem, hex detail, and the state active when it was recorded (null when
// none). Always returns None.
TableExportError export_faults_ndjson(const AnomalyEngine& anomalies,
                                      std::string& out);

}  // namespace blackspan

#endif  // BLACKSPAN_EXPORT_H
