#ifndef BLACKSPAN_SCHEMA_H
#define BLACKSPAN_SCHEMA_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "blackspan/frame.h"

namespace blackspan {

// The schema subsystem describes the layout of the fixed-width samples that a
// sample-block frame carries for a given channel. A schema-def frame announces
// a channel's field list; later sample-blocks are just packed rows that a
// resolved schema turns into typed values. Schemas are content-identified: two
// channels that declare the same fields share a layout, and a channel that
// changes its fields gains a new version while its old versions stay
// resolvable for already-recorded samples.

// Upper bound on a resolved sample stride. A single sample must fit comfortably
// so that a malformed definition cannot demand an unreasonable row size.
constexpr size_t kMaxSchemaStride = 4096;

// Upper bound on a channel or field name length. The wire encodes lengths in a
// single byte, but a stricter cap keeps definitions readable and bounds the
// work a decoder performs on hostile input.
constexpr size_t kMaxSchemaNameLen = 64;

// Scalar types a channel field may hold. The numeric values are the on-wire
// encoding used by schema-def frames and must stay stable.
enum class FieldType : uint8_t {
  U8 = 1,
  I8 = 2,
  U16 = 3,
  I16 = 4,
  U32 = 5,
  I32 = 6,
  U64 = 7,
  I64 = 8,
  F32 = 9,
  F64 = 10,
};

// Size in bytes of a field of the given type. Returns 0 for an unrecognized
// value so callers can treat it as invalid.
size_t field_type_size(FieldType t);

// Short lowercase mnemonic ("u8", "i16", "f64"). Never null.
const char* field_type_name(FieldType t);

// True for the signed integer types (I8/I16/I32/I64). Floating-point types are
// reported by field_type_is_float, not here.
bool field_type_is_signed(FieldType t);

// True for F32/F64.
bool field_type_is_float(FieldType t);

// Parses an on-wire type byte, returning nullopt for anything outside 1..10.
std::optional<FieldType> parse_field_type(uint8_t v);

// Error codes for schema decoding, validation, and registration. Errors are
// values; nothing in this subsystem throws.
enum class SchemaError : uint8_t {
  None = 0,
  Truncated,           // payload ended before a declared field or name
  BadFieldType,        // a field type byte was not a known FieldType
  ZeroFields,          // a schema declared no fields
  EmptyName,           // a field name was empty
  DuplicateFieldName,  // two fields shared a name
  NameTooLong,         // a channel or field name exceeded kMaxSchemaNameLen
  StrideTooLarge,      // the resolved sample stride exceeded kMaxSchemaStride
  TrailingBytes,       // extra bytes followed a complete definition
  NotSchemaFrame,      // ingest_frame was handed a non-schema-def frame
};

// Human-readable form of a SchemaError. Never null.
const char* schema_error_str(SchemaError e);

// One field within a channel schema. The offset is the byte position of the
// field within a packed sample row and is derived from the field order.
struct SchemaField {
  std::string name;
  FieldType type = FieldType::U8;
  size_t offset = 0;
};

// A resolved channel layout: an ordered, packed list of typed fields plus the
// per-sample stride they occupy. Fields are packed with no padding, each at its
// natural size, in declaration order.
class ChannelSchema {
 public:
  uint16_t channel_id = 0;
  uint16_t version = 0;  // assigned by the registry; 0 until registered
  std::string name;
  std::vector<SchemaField> fields;
  size_t stride = 0;

  // Recomputes each field's offset and the overall stride from the current
  // field list. Call after building or editing the field list.
  void recompute_layout();

  // Checks structural invariants: at least one field, non-empty and unique
  // field names, names within the length cap, recognized field types, and a
  // stride within kMaxSchemaStride. Returns SchemaError::None when valid. The
  // stride is checked against a freshly summed value, so validate() is correct
  // even if recompute_layout() has not been called since the last edit.
  SchemaError validate(size_t max_stride = kMaxSchemaStride) const;

  // Returns the field with the given name, or nullptr if none matches.
  const SchemaField* field_by_name(const std::string& field_name) const;

  // Returns the index of the named field, or nullopt if none matches.
  std::optional<size_t> field_index(const std::string& field_name) const;

  // True when this layout matches another's: same channel name and the same
  // ordered field names and types. Version, channel_id, and derived offsets are
  // deliberately ignored.
  bool same_layout(const ChannelSchema& other) const;

  // A single-line human-readable summary.
  std::string describe() const;
};

// The outcome of decoding a schema-def payload: an error code plus, on success,
// the resolved schema and the version the recorder declared on the wire (which
// is advisory; the registry assigns its own authoritative versions).
struct SchemaDecodeResult {
  SchemaError error = SchemaError::None;
  ChannelSchema schema;
  uint8_t declared_version = 0;

  bool ok() const { return error == SchemaError::None; }
};

// Decodes a schema-def frame payload. Grammar (all little-endian):
//   [channel_id u16][declared_version u8][field_count u8][name_len u8]
//   [name bytes] then per field: [field_type u8][name_len u8][name bytes]
// Robust to truncation, unknown field types, zero fields, over-long names, and
// trailing bytes; each is reported as a specific SchemaError.
SchemaDecodeResult decode_channel_schema(const uint8_t* payload, size_t len);

// A versioned registry of channel schemas. Each channel_id owns a history of
// layouts. The first layout registered for a channel becomes version 1;
// re-registering an identical layout is a no-op that returns the existing
// entry; re-registering a different layout appends a new version. Older
// versions remain resolvable so that samples recorded against them can still be
// decoded.
class SchemaRegistry {
 public:
  // Registers a schema under its channel_id. Returns the canonical stored
  // schema (with its assigned version) on success, or nullptr on a validation
  // failure, in which case *err (if non-null) receives the reason. On success
  // *err is set to SchemaError::None. Returned pointers stay valid for the
  // lifetime of the registry.
  const ChannelSchema* register_schema(const ChannelSchema& s,
                                       SchemaError* err = nullptr);

  // Most recently registered version for a channel, or nullptr if unknown.
  const ChannelSchema* latest(uint16_t channel_id) const;

  // A specific version (1-based) of a channel, or nullptr if absent.
  const ChannelSchema* get(uint16_t channel_id, uint16_t version) const;

  // True if any version of the channel is registered.
  bool contains(uint16_t channel_id) const;

  // Number of distinct channels registered.
  size_t count() const;

  // Total number of stored versions across all channels.
  size_t total_versions() const;

  // The versions known for a channel, ascending (e.g. {1, 2, 3}); empty if the
  // channel is unknown.
  std::vector<uint16_t> versions(uint16_t channel_id) const;

  // All registered channel ids, ascending.
  std::vector<uint16_t> channels() const;

  // Decodes a schema-def frame's payload and registers the result. Returns
  // NotSchemaFrame for a frame of any other record type, the decode error for a
  // malformed payload, or the registration result otherwise.
  SchemaError ingest_frame(const Frame& f);

 private:
  // channel_id -> ascending version history (index 0 is version 1). A deque is
  // used so that pointers to stored schemas stay valid as new versions append.
  std::map<uint16_t, std::deque<ChannelSchema>> channels_;
};

// A read-only typed view over a single packed sample row. The view borrows both
// the schema and the sample bytes; neither is copied and both must outlive it.
// Every accessor is bounds-checked against the field count, the field type, and
// the length of the borrowed span, returning nullopt rather than reading out of
// range.
class SampleView {
 public:
  SampleView(const ChannelSchema& schema, const uint8_t* data, size_t len)
      : schema_(schema), data_(data), len_(len) {}

  // True when the borrowed span is at least one full sample stride.
  bool complete() const { return len_ >= schema_.stride; }

  size_t field_count() const { return schema_.fields.size(); }
  const ChannelSchema& schema() const { return schema_; }

  std::optional<uint8_t> get_u8(size_t field_index) const;
  std::optional<int8_t> get_i8(size_t field_index) const;
  std::optional<uint16_t> get_u16(size_t field_index) const;
  std::optional<int16_t> get_i16(size_t field_index) const;
  std::optional<uint32_t> get_u32(size_t field_index) const;
  std::optional<int32_t> get_i32(size_t field_index) const;
  std::optional<uint64_t> get_u64(size_t field_index) const;
  std::optional<int64_t> get_i64(size_t field_index) const;
  std::optional<float> get_f32(size_t field_index) const;
  std::optional<double> get_f64(size_t field_index) const;

  // Reads whichever type the field holds and widens it to double. Signed
  // integers are sign-extended; unsigned integers are zero-extended. Returns
  // nullopt for an out-of-range index or a span too short for the field.
  std::optional<double> get_as_double(size_t field_index) const;

 private:
  // Reads n little-endian bytes at the given field's offset into a uint64_t,
  // checking the field index, type, and span length. On success sets *raw and
  // returns true.
  bool read_raw(size_t field_index, FieldType expect, uint64_t* raw) const;

  const ChannelSchema& schema_;
  const uint8_t* data_;
  size_t len_;
};

}  // namespace blackspan

#endif  // BLACKSPAN_SCHEMA_H
