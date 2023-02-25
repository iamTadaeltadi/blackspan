#ifndef BLACKSPAN_ASSEMBLE_H
#define BLACKSPAN_ASSEMBLE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "blackspan/frame.h"
#include "blackspan/schema.h"

// The assemble subsystem turns a human-readable recording script into the exact
// bytes a device recorder would have written. It is the inverse of the decode
// pipeline and exists for the jobs that a decoder alone cannot do: authoring
// regression fixtures, reproducing a field failure reported as a description
// rather than a file, and checking that a recording with a known shape decodes
// the way the format documents.
//
// A script is line-oriented. Blank lines and lines whose first non-blank
// character is '#' are ignored. Every other line is a directive: a name
// followed by whitespace-separated arguments, each either a bare value or a
// key=value pair whose value may be a double-quoted string.
//
// Directives fall into three groups. Stream-state directives (device, session,
// ts, advance, flags) set the frame-header fields that following records
// inherit, so a script does not repeat them on every line. Record directives
// (session-begin, session-end, schema, samples/row/end, state, config, fault,
// marker, checkpoint, reconnect, derived, export, raw) each emit one frame.
// The damage directive perturbs the frame most recently emitted so that a
// script can produce the corrupt recordings a decoder must survive.
//
// Assembly never throws and never stops at the first problem: each malformed
// directive is reported as a diagnostic carrying its line number, and the
// remaining lines are still processed so that one run reports every error in a
// script. A script that produces any diagnostic yields no bytes.

namespace blackspan {

// Why a directive was rejected. Errors are values; nothing here throws.
enum class AssembleError : uint8_t {
  None = 0,
  UnknownDirective,      // the first token named no known directive
  UnknownArgument,       // a key=value pair the directive does not accept
  MissingArgument,       // a required argument was absent
  DuplicateArgument,     // the same key appeared twice on one directive
  BadArgument,           // an argument's value did not parse for its type
  ValueOutOfRange,       // a parsed value did not fit the wire field
  UnterminatedString,    // a quoted value had no closing quote
  BadEscape,             // an unknown backslash escape in a quoted value
  BadHex,                // a hex payload had an odd length or a non-hex digit
  UnknownFieldType,      // a schema field named an unknown scalar type
  DuplicateFieldName,    // a schema declared the same field name twice
  EmptySchema,           // a schema declared no fields
  UnknownChannel,        // a samples block named an undeclared channel
  UnknownVersion,        // a samples block named an undeclared schema version
  RowOutsideBlock,       // a row directive appeared outside samples..end
  BlockInsideBlock,      // a samples directive appeared inside a samples block
  DirectiveInsideBlock,  // a non-row directive appeared inside a samples block
  UnterminatedBlock,     // the script ended with a samples block still open
  StrayEnd,              // an end directive closed nothing
  RowFieldCount,         // a row supplied the wrong number of values
  EmptyBlock,            // a samples block carried no rows
  PayloadTooLong,        // an encoded payload exceeded kMaxPayload
  NothingToDamage,       // a damage directive preceded any frame
  BadDamageMode,         // a damage directive named an unknown mode
};

// Human-readable form of an AssembleError. Never null.
const char* assemble_error_str(AssembleError e);

// One rejected directive: the 1-based script line, the reason, and a detail
// string naming the offending token or key.
struct AssembleDiagnostic {
  size_t line = 0;
  AssembleError error = AssembleError::None;
  std::string detail;
};

// A single-line rendering of a diagnostic, e.g.
// `line 12: bad-argument (device=zzz)`.
std::string describe_assemble_diagnostic(const AssembleDiagnostic& d);

// The outcome of assembling a script: the encoded recording, how many frames it
// holds, and every diagnostic raised. `bytes` is empty whenever `errors` is not.
struct AssembleResult {
  std::vector<uint8_t> bytes;
  size_t frame_count = 0;
  std::vector<AssembleDiagnostic> errors;

  bool ok() const { return errors.empty(); }
};

// Assembles a whole script. Line endings may be LF or CRLF.
AssembleResult assemble_script(const std::string& text);

// ---- script lexer (unit-tested directly) --------------------------------

// One argument token from a directive line. `key` is empty for a positional
// argument. `quoted` records whether the value arrived double-quoted, which
// distinguishes an empty string from a missing one.
struct ScriptArg {
  std::string key;
  std::string value;
  bool quoted = false;
};

// A tokenized directive line: the directive name, its arguments in order, and
// the 1-based line number it came from.
struct ScriptLine {
  size_t line = 0;
  std::string name;
  std::vector<ScriptArg> args;
};

// Splits one script line into a directive name and arguments. Returns false for
// a blank or comment line (nothing is written to out). On a malformed line the
// function still returns true and reports the reason through *err so the caller
// can attach a line number.
bool lex_script_line(const std::string& text, ScriptLine& out,
                     AssembleError* err);

// Looks up an argument by key. Returns nullptr when the key is absent.
const ScriptArg* find_arg(const ScriptLine& line, const char* key);

// ---- value parsers (unit-tested directly) -------------------------------

// Parses an unsigned integer, accepting decimal, 0x-prefixed hex, and
// 0b-prefixed binary. Rejects an empty string, a sign, trailing characters, and
// anything above `max`.
bool parse_uint(const std::string& s, uint64_t max, uint64_t& out);

// Parses a signed integer in the inclusive range [min, max], accepting an
// optional leading '-' before any base the unsigned parser accepts.
bool parse_int(const std::string& s, int64_t min, int64_t max, int64_t& out);

// Parses a floating-point value, accepting the forms std::strtod does plus the
// bare words "nan", "inf", and "-inf". Rejects trailing characters.
bool parse_real(const std::string& s, double& out);

// Parses a scalar field type mnemonic ("u8".."f64"), or nullopt.
std::optional<FieldType> parse_field_type_name(const std::string& s);

// Parses a fault severity name ("info", "warning", "error", "critical") or a
// bare number 0..3. Returns nullopt for anything else.
std::optional<uint8_t> parse_severity_name(const std::string& s);

// Decodes an even-length string of hex digit pairs into bytes. Returns false on
// an odd length or a non-hex digit.
bool parse_hex_bytes(const std::string& s, std::vector<uint8_t>& out);

// ---- assembler state (exposed for tests) --------------------------------

// Tracks the schemas a script has declared so that later samples blocks can
// encode their rows against the right layout. Version numbering mirrors the
// decoder's SchemaRegistry: the first layout for a channel is version 1,
// re-declaring an identical layout reuses it, and a changed layout appends a
// new version.
class ScriptSchemas {
 public:
  // Records a declared layout and returns the version assigned to it.
  uint16_t declare(const ChannelSchema& s);

  // The layout for a channel and version, or nullptr if either is unknown.
  const ChannelSchema* get(uint16_t channel_id, uint16_t version) const;

  // The most recent version declared for a channel, or 0 if unknown.
  uint16_t latest_version(uint16_t channel_id) const;

  size_t channel_count() const { return channels_.size(); }

 private:
  std::map<uint16_t, std::vector<ChannelSchema>> channels_;
};

// ---- record encoders (unit-tested directly) -----------------------------

// Encodes one packed sample row. `values` holds one already-parsed value per
// schema field, as text; each is converted according to the field's type.
// Returns false if a value does not parse for its field, writing the offending
// field index to *bad_field when non-null.
bool encode_sample_row(const ChannelSchema& schema,
                       const std::vector<std::string>& values,
                       std::vector<uint8_t>& out, size_t* bad_field);

// Encodes a schema-def (0x03) payload for a declared layout.
void encode_schema_payload(const ChannelSchema& schema, uint16_t version,
                           std::vector<uint8_t>& out);

}  // namespace blackspan

#endif  // BLACKSPAN_ASSEMBLE_H
