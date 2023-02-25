#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "blackspan/assemble.h"

namespace blackspan {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// True for the characters a bare (unquoted) argument value may contain. The set
// is deliberately generous — it holds every printable character except the
// whitespace that separates arguments, the '=' that splits a key from its
// value, the '"' that opens a quoted value, and the '#' that opens a comment.
bool is_bare_char(char c) {
  unsigned char u = static_cast<unsigned char>(c);
  if (u < 0x21 || u > 0x7e) return false;
  return c != '=' && c != '"' && c != '#';
}

// True for the characters a directive or argument name may contain.
bool is_name_char(char c) {
  unsigned char u = static_cast<unsigned char>(c);
  return std::isalnum(u) != 0 || c == '-' || c == '_' || c == '.';
}

// Reads a double-quoted value starting at text[i] (which must be the opening
// quote), appending the unescaped contents to out and leaving i just past the
// closing quote. Recognizes \\ \" \n \r \t \0 and \xHH.
bool read_quoted(const std::string& text, size_t& i, std::string& out,
                 AssembleError* err) {
  ++i;  // opening quote
  while (i < text.size()) {
    char c = text[i];
    if (c == '"') {
      ++i;
      return true;
    }
    if (c != '\\') {
      out.push_back(c);
      ++i;
      continue;
    }
    if (i + 1 >= text.size()) break;
    char e = text[i + 1];
    i += 2;
    switch (e) {
      case '\\': out.push_back('\\'); break;
      case '"': out.push_back('"'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case '0': out.push_back('\0'); break;
      case 'x': {
        if (i + 1 >= text.size()) {
          if (err) *err = AssembleError::BadEscape;
          return false;
        }
        std::string hex = text.substr(i, 2);
        std::vector<uint8_t> bytes;
        if (!parse_hex_bytes(hex, bytes)) {
          if (err) *err = AssembleError::BadEscape;
          return false;
        }
        out.push_back(static_cast<char>(bytes[0]));
        i += 2;
        break;
      }
      default:
        if (err) *err = AssembleError::BadEscape;
        return false;
    }
  }
  if (err) *err = AssembleError::UnterminatedString;
  return false;
}

// Reads one argument (bare, quoted, or key=value) at text[i].
bool read_arg(const std::string& text, size_t& i, ScriptArg& out,
              AssembleError* err) {
  // A leading quote makes the whole argument a positional string.
  if (text[i] == '"') return read_quoted(text, i, out.value, err);

  std::string first;
  while (i < text.size() && is_bare_char(text[i])) first.push_back(text[i++]);
  if (first.empty()) {
    if (err) *err = AssembleError::BadArgument;
    return false;
  }

  if (i < text.size() && text[i] == '=') {
    ++i;
    out.key = first;
    if (i < text.size() && text[i] == '"') {
      out.quoted = true;
      return read_quoted(text, i, out.value, err);
    }
    while (i < text.size() && is_bare_char(text[i])) out.value.push_back(text[i++]);
    return true;
  }

  out.value = first;
  return true;
}

}  // namespace

const char* assemble_error_str(AssembleError e) {
  switch (e) {
    case AssembleError::None: return "none";
    case AssembleError::UnknownDirective: return "unknown-directive";
    case AssembleError::UnknownArgument: return "unknown-argument";
    case AssembleError::MissingArgument: return "missing-argument";
    case AssembleError::DuplicateArgument: return "duplicate-argument";
    case AssembleError::BadArgument: return "bad-argument";
    case AssembleError::ValueOutOfRange: return "value-out-of-range";
    case AssembleError::UnterminatedString: return "unterminated-string";
    case AssembleError::BadEscape: return "bad-escape";
    case AssembleError::BadHex: return "bad-hex";
    case AssembleError::UnknownFieldType: return "unknown-field-type";
    case AssembleError::DuplicateFieldName: return "duplicate-field-name";
    case AssembleError::EmptySchema: return "empty-schema";
    case AssembleError::UnknownChannel: return "unknown-channel";
    case AssembleError::UnknownVersion: return "unknown-version";
    case AssembleError::RowOutsideBlock: return "row-outside-block";
    case AssembleError::BlockInsideBlock: return "block-inside-block";
    case AssembleError::DirectiveInsideBlock: return "directive-inside-block";
    case AssembleError::UnterminatedBlock: return "unterminated-block";
    case AssembleError::StrayEnd: return "stray-end";
    case AssembleError::RowFieldCount: return "row-field-count";
    case AssembleError::EmptyBlock: return "empty-block";
    case AssembleError::PayloadTooLong: return "payload-too-long";
    case AssembleError::NothingToDamage: return "nothing-to-damage";
    case AssembleError::BadDamageMode: return "bad-damage-mode";
  }
  return "unknown";
}

std::string describe_assemble_diagnostic(const AssembleDiagnostic& d) {
  std::string s = "line " + std::to_string(d.line) + ": ";
  s += assemble_error_str(d.error);
  if (!d.detail.empty()) {
    s += " (";
    s += d.detail;
    s += ")";
  }
  return s;
}

bool lex_script_line(const std::string& text, ScriptLine& out,
                     AssembleError* err) {
  if (err) *err = AssembleError::None;

  size_t i = 0;
  while (i < text.size() && is_space(text[i])) ++i;
  if (i >= text.size() || text[i] == '#') return false;

  std::string name;
  while (i < text.size() && is_name_char(text[i])) name.push_back(text[i++]);
  if (name.empty()) {
    if (err) *err = AssembleError::UnknownDirective;
    out.name = text.substr(i, 16);
    return true;
  }
  out.name = name;

  while (i < text.size()) {
    while (i < text.size() && is_space(text[i])) ++i;
    if (i >= text.size() || text[i] == '#') break;
    ScriptArg arg;
    if (!read_arg(text, i, arg, err)) return true;
    out.args.push_back(std::move(arg));
  }
  return true;
}

const ScriptArg* find_arg(const ScriptLine& line, const char* key) {
  for (const ScriptArg& a : line.args) {
    if (a.key == key) return &a;
  }
  return nullptr;
}

bool parse_uint(const std::string& s, uint64_t max, uint64_t& out) {
  if (s.empty()) return false;

  int base = 10;
  size_t i = 0;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    i = 2;
  } else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
    base = 2;
    i = 2;
  }
  if (i >= s.size()) return false;

  uint64_t acc = 0;
  for (; i < s.size(); ++i) {
    char c = s[i];
    if (c == '_') continue;  // digit grouping, e.g. 1_000
    int digit;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (c >= 'a' && c <= 'f')
      digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      digit = c - 'A' + 10;
    else
      return false;
    if (digit >= base) return false;
    if (acc > (UINT64_MAX - static_cast<uint64_t>(digit)) /
                  static_cast<uint64_t>(base)) {
      return false;
    }
    acc = acc * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
  }
  if (acc > max) return false;
  out = acc;
  return true;
}

bool parse_int(const std::string& s, int64_t min, int64_t max, int64_t& out) {
  if (s.empty()) return false;
  bool negative = s[0] == '-';
  std::string digits = negative ? s.substr(1) : s;
  uint64_t mag = 0;
  // The magnitude of the most negative int64 does not fit in an int64, so parse
  // into a uint64 first and only then apply the sign.
  if (!parse_uint(digits, negative ? 0x8000000000000000ull : INT64_MAX, mag)) {
    return false;
  }
  int64_t v;
  if (negative) {
    if (mag == 0x8000000000000000ull)
      v = INT64_MIN;
    else
      v = -static_cast<int64_t>(mag);
  } else {
    v = static_cast<int64_t>(mag);
  }
  if (v < min || v > max) return false;
  out = v;
  return true;
}

bool parse_real(const std::string& s, double& out) {
  if (s.empty()) return false;
  errno = 0;
  const char* start = s.c_str();
  char* end = nullptr;
  double v = std::strtod(start, &end);
  if (end == start) return false;
  while (*end == ' ' || *end == '\t') ++end;
  if (*end != '\0') return false;
  out = v;
  return true;
}

std::optional<FieldType> parse_field_type_name(const std::string& s) {
  if (s == "u8") return FieldType::U8;
  if (s == "i8") return FieldType::I8;
  if (s == "u16") return FieldType::U16;
  if (s == "i16") return FieldType::I16;
  if (s == "u32") return FieldType::U32;
  if (s == "i32") return FieldType::I32;
  if (s == "u64") return FieldType::U64;
  if (s == "i64") return FieldType::I64;
  if (s == "f32") return FieldType::F32;
  if (s == "f64") return FieldType::F64;
  return std::nullopt;
}

std::optional<uint8_t> parse_severity_name(const std::string& s) {
  if (s == "info") return 0;
  if (s == "warning" || s == "warn") return 1;
  if (s == "error") return 2;
  if (s == "critical" || s == "crit") return 3;
  uint64_t v = 0;
  if (parse_uint(s, 3, v)) return static_cast<uint8_t>(v);
  return std::nullopt;
}

bool parse_hex_bytes(const std::string& s, std::vector<uint8_t>& out) {
  if (s.size() % 2 != 0) return false;
  out.clear();
  out.reserve(s.size() / 2);
  for (size_t i = 0; i < s.size(); i += 2) {
    int hi, lo;
    auto digit = [](char c, int& v) -> bool {
      if (c >= '0' && c <= '9') {
        v = c - '0';
        return true;
      }
      if (c >= 'a' && c <= 'f') {
        v = c - 'a' + 10;
        return true;
      }
      if (c >= 'A' && c <= 'F') {
        v = c - 'A' + 10;
        return true;
      }
      return false;
    };
    if (!digit(s[i], hi) || !digit(s[i + 1], lo)) return false;
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

}  // namespace blackspan
