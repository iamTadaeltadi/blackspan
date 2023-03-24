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

constexpr size_t kMaxSchemaStride = 4096;

constexpr size_t kMaxSchemaNameLen = 64;

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

size_t field_type_size(FieldType t);

const char* field_type_name(FieldType t);

bool field_type_is_signed(FieldType t);

bool field_type_is_float(FieldType t);

std::optional<FieldType> parse_field_type(uint8_t v);

enum class SchemaError : uint8_t {
  None = 0,
  Truncated,
  BadFieldType,
  ZeroFields,
  EmptyName,
  DuplicateFieldName,
  NameTooLong,
  StrideTooLarge,
  TrailingBytes,
  NotSchemaFrame,
};

const char* schema_error_str(SchemaError e);

struct SchemaField {
  std::string name;
  FieldType type = FieldType::U8;
  size_t offset = 0;
};

class ChannelSchema {
 public:
  uint16_t channel_id = 0;
  uint16_t version = 0;
  std::string name;
  std::vector<SchemaField> fields;
  size_t stride = 0;

  void recompute_layout();

  SchemaError validate(size_t max_stride = kMaxSchemaStride) const;

  const SchemaField* field_by_name(const std::string& field_name) const;

  std::optional<size_t> field_index(const std::string& field_name) const;

  bool same_layout(const ChannelSchema& other) const;

  std::string describe() const;
};

struct SchemaDecodeResult {
  SchemaError error = SchemaError::None;
  ChannelSchema schema;
  uint8_t declared_version = 0;

  bool ok() const { return error == SchemaError::None; }
};

SchemaDecodeResult decode_channel_schema(const uint8_t* payload, size_t len);

class SchemaRegistry {
 public:

  const ChannelSchema* register_schema(const ChannelSchema& s,
                                       SchemaError* err = nullptr);

  const ChannelSchema* latest(uint16_t channel_id) const;

  const ChannelSchema* get(uint16_t channel_id, uint16_t version) const;

  bool contains(uint16_t channel_id) const;

  size_t count() const;

  size_t total_versions() const;

  std::vector<uint16_t> versions(uint16_t channel_id) const;

  std::vector<uint16_t> channels() const;

  SchemaError ingest_frame(const Frame& f);

 private:

  std::map<uint16_t, std::deque<ChannelSchema>> channels_;
};

class SampleView {
 public:
  SampleView(const ChannelSchema& schema, const uint8_t* data, size_t len)
      : schema_(schema), data_(data), len_(len) {}

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

  std::optional<double> get_as_double(size_t field_index) const;

 private:

  bool read_raw(size_t field_index, FieldType expect, uint64_t* raw) const;

  const ChannelSchema& schema_;
  const uint8_t* data_;
  size_t len_;
};

}

#endif
