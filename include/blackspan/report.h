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

// The report subsystem is the consumer at the end of the decode pipeline: it
// renders an already-reconstructed model to deterministic human- and
// machine-readable forms. It decodes no wire bytes (framing and the domain
// subsystems have already done that) and mutates none of the model it is
// handed; every renderer is a pure function of its input, so rendering the same
// model twice yields byte-identical output. Nothing here throws.

namespace blackspan {

// The set of reconstructed views a report draws from. Every member is optional:
// a null pointer means that subsystem was not run, and the matching section is
// omitted rather than rendered empty. The model borrows all four views and must
// not outlive them.
struct ReportModel {
  const std::vector<Session>* sessions = nullptr;
  const SchemaRegistry* schemas = nullptr;
  const ChannelStore* channels = nullptr;
  const AnomalyEngine* anomalies = nullptr;
};

// Section selector bits for a section_mask. A section is rendered only when its
// bit is set and its backing model pointer is non-null. kSectionAll selects
// every section; unknown high bits are ignored.
enum ReportSection : uint16_t {
  kSectionSessions = 1u << 0,
  kSectionSchemas = 1u << 1,
  kSectionChannels = 1u << 2,
  kSectionAnomalies = 1u << 3,
};
constexpr uint16_t kSectionAll = 0xFFFFu;

// The output format. The numeric values are the on-wire encoding used by the
// export-request record and must stay stable.
enum class ReportFormat : uint8_t {
  Text = 0,
  Json = 1,
  Dot = 2,
};

// Human-readable name of a format ("text"/"json"/"dot"). Never null.
const char* report_format_name(ReportFormat f);

// Renderers. Each returns a freshly built string and does not modify the model.
// section_mask restricts which sections are emitted and defaults to all of
// them; a mask with no known bits set yields a document with no sections (a
// bare header for text, an empty object for JSON, an empty digraph for dot).
std::string render_text(const ReportModel& model,
                        uint16_t section_mask = kSectionAll);
std::string render_json(const ReportModel& model,
                        uint16_t section_mask = kSectionAll);
std::string render_dot(const ReportModel& model,
                       uint16_t section_mask = kSectionAll);

// Dispatches to the renderer for the requested format.
std::string render(const ReportModel& model, ReportFormat format,
                   uint16_t section_mask = kSectionAll);

// ---- export-request record (0x0C) ---------------------------------------

// Fixed size of an export-request payload: a format byte followed by a 16-bit
// section mask, little-endian.
constexpr size_t kExportRequestSize = 3;

// A decoded export-request: which format to render and which sections to
// include. A section_mask of 0 is a producer's "default" and is interpreted by
// the render(model, request) overload as "all sections".
struct ExportRequest {
  ReportFormat format = ReportFormat::Text;
  uint16_t section_mask = 0;
};

// Error codes for export-request decoding. Errors are values; nothing here
// throws.
enum class ExportError : uint8_t {
  None = 0,
  Truncated,      // payload shorter than kExportRequestSize
  BadFormat,      // the format byte was not 0, 1, or 2
  TrailingBytes,  // extra bytes followed the fixed payload
};

// Human-readable form of an ExportError. Never null.
const char* export_error_str(ExportError e);

// Maps a format byte (0=text, 1=json, 2=dot) to a ReportFormat, or nullopt for
// any other value.
std::optional<ReportFormat> format_from_byte(uint8_t v);

// export-request (0x0C): [format u8][section_mask u16]  (little-endian).
// Decodes the fixed 3-byte payload into out. Returns Truncated on a short
// payload, BadFormat on an unknown format byte, TrailingBytes when more than
// three bytes are present, and None on success.
ExportError decode_export_request(const uint8_t* payload, size_t len,
                                  ExportRequest& out);

// Renders a model according to a decoded export request. A section_mask of 0 is
// treated as kSectionAll.
std::string render(const ReportModel& model, const ExportRequest& request);

// ---- formatting helpers (unit-tested directly) ---------------------------

// Locale-independent, stable formatting of a double via "%.6g". Non-finite
// values render as "nan"/"inf"/"-inf". Used by the text and dot renderers so
// numeric output is identical across formats and runs.
std::string format_number(double v);

// Escapes a string for a Graphviz double-quoted label: backslash and double
// quote are backslash-escaped, a newline becomes the two-character sequence
// \n, and every other control character below 0x20 becomes a space, so a label
// can never introduce a stray quote, backslash, or line break into the graph.
std::string escape_dot(const std::string& s);

}  // namespace blackspan

#endif  // BLACKSPAN_REPORT_H
