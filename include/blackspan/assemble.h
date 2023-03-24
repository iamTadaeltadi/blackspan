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

namespace blackspan {

enum class AssembleError : uint8_t {
  None = 0,
  UnknownDirective,
  UnknownArgument,
  MissingArgument,
  DuplicateArgument,
  BadArgument,
  ValueOutOfRange,
  UnterminatedString,
  BadEscape,
  BadHex,
  UnknownFieldType,
  DuplicateFieldName,
  EmptySchema,
  UnknownChannel,
  UnknownVersion,
  RowOutsideBlock,
  BlockInsideBlock,
  DirectiveInsideBlock,
  UnterminatedBlock,
  StrayEnd,
  RowFieldCount,
  EmptyBlock,
  PayloadTooLong,
  NothingToDamage,
  BadDamageMode,
};

const char* assemble_error_str(AssembleError e);

struct AssembleDiagnostic {
  size_t line = 0;
  AssembleError error = AssembleError::None;
  std::string detail;
};

std::string describe_assemble_diagnostic(const AssembleDiagnostic& d);

struct AssembleResult {
  std::vector<uint8_t> bytes;
  size_t frame_count = 0;
  std::vector<AssembleDiagnostic> errors;

  bool ok() const { return errors.empty(); }
};

AssembleResult assemble_script(const std::string& text);

struct ScriptArg {
  std::string key;
  std::string value;
  bool quoted = false;
};

struct ScriptLine {
  size_t line = 0;
  std::string name;
  std::vector<ScriptArg> args;
};

bool lex_script_line(const std::string& text, ScriptLine& out,
                     AssembleError* err);

const ScriptArg* find_arg(const ScriptLine& line, const char* key);

bool parse_uint(const std::string& s, uint64_t max, uint64_t& out);

bool parse_int(const std::string& s, int64_t min, int64_t max, int64_t& out);

bool parse_real(const std::string& s, double& out);

std::optional<FieldType> parse_field_type_name(const std::string& s);

std::optional<uint8_t> parse_severity_name(const std::string& s);

bool parse_hex_bytes(const std::string& s, std::vector<uint8_t>& out);

class ScriptSchemas {
 public:

  uint16_t declare(const ChannelSchema& s);

  const ChannelSchema* get(uint16_t channel_id, uint16_t version) const;

  uint16_t latest_version(uint16_t channel_id) const;

  size_t channel_count() const { return channels_.size(); }

 private:
  std::map<uint16_t, std::vector<ChannelSchema>> channels_;
};

bool encode_sample_row(const ChannelSchema& schema,
                       const std::vector<std::string>& values,
                       std::vector<uint8_t>& out, size_t* bad_field);

void encode_schema_payload(const ChannelSchema& schema, uint16_t version,
                           std::vector<uint8_t>& out);

}

#endif
