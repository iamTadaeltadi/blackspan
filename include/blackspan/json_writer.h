#ifndef BLACKSPAN_JSON_WRITER_H
#define BLACKSPAN_JSON_WRITER_H

#include <cstddef>
#include <string>
#include <vector>

namespace blackspan {

// Escapes a string for embedding in a JSON double-quoted literal (without the
// surrounding quotes). The two structural characters " and \ are backslash-
// escaped, the control characters that JSON names get their short escapes
// (\b \f \n \r \t), and any other control character below 0x20 becomes a
// \u00XX sequence. Bytes 0x20 and above (including UTF-8 continuation bytes)
// pass through unchanged so already-valid UTF-8 text round-trips byte for byte.
std::string escape_json(const std::string& s);

// A minimal streaming JSON writer with no third-party dependency and no
// intermediate value tree. Callers drive it with begin/end object and array
// calls, keys, and typed values; the writer tracks container nesting so that
// element and member separators are inserted correctly. Output is compact (no
// insignificant whitespace) and therefore byte-deterministic for a given call
// sequence, which is what the report renderers rely on.
class JsonWriter {
 public:
  void begin_object();
  void end_object();
  void begin_array();
  void end_array();

  // Writes a member key inside the current object. Must be followed by exactly
  // one value call, which becomes the member's value.
  void key(const std::string& k);

  // Value writers. Each emits one value in the current context: a top-level
  // value, an array element, or the value half of a preceding key().
  void string_value(const std::string& v);
  void int_value(long long v);
  void uint_value(unsigned long long v);
  void double_value(double v);
  void bool_value(bool v);
  void null_value();

  // The accumulated document.
  const std::string& str() const { return out_; }

 private:
  // One open container: 'o' for an object, 'a' for an array, plus the number of
  // elements (or members) written into it so far.
  struct Level {
    char kind;
    size_t count;
  };

  // Emits a comma before the next value when one is required, and clears the
  // "a key was just written" state so a key's value is not comma-separated from
  // its key.
  void before_value();

  std::string out_;
  std::vector<Level> stack_;
  bool pending_key_ = false;
};

}  // namespace blackspan

#endif  // BLACKSPAN_JSON_WRITER_H
