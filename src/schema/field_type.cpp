#include "blackspan/schema.h"

namespace blackspan {

size_t field_type_size(FieldType t) {
  switch (t) {
    case FieldType::U8:
    case FieldType::I8:
      return 1;
    case FieldType::U16:
    case FieldType::I16:
      return 2;
    case FieldType::U32:
    case FieldType::I32:
    case FieldType::F32:
      return 4;
    case FieldType::U64:
    case FieldType::I64:
    case FieldType::F64:
      return 8;
  }
  return 0;
}

const char* field_type_name(FieldType t) {
  switch (t) {
    case FieldType::U8:
      return "u8";
    case FieldType::I8:
      return "i8";
    case FieldType::U16:
      return "u16";
    case FieldType::I16:
      return "i16";
    case FieldType::U32:
      return "u32";
    case FieldType::I32:
      return "i32";
    case FieldType::U64:
      return "u64";
    case FieldType::I64:
      return "i64";
    case FieldType::F32:
      return "f32";
    case FieldType::F64:
      return "f64";
  }
  return "unknown";
}

bool field_type_is_signed(FieldType t) {
  switch (t) {
    case FieldType::I8:
    case FieldType::I16:
    case FieldType::I32:
    case FieldType::I64:
      return true;
    default:
      return false;
  }
}

bool field_type_is_float(FieldType t) {
  return t == FieldType::F32 || t == FieldType::F64;
}

std::optional<FieldType> parse_field_type(uint8_t v) {
  if (v >= static_cast<uint8_t>(FieldType::U8) &&
      v <= static_cast<uint8_t>(FieldType::F64)) {
    return static_cast<FieldType>(v);
  }
  return std::nullopt;
}

const char* schema_error_str(SchemaError e) {
  switch (e) {
    case SchemaError::None:
      return "none";
    case SchemaError::Truncated:
      return "truncated";
    case SchemaError::BadFieldType:
      return "bad-field-type";
    case SchemaError::ZeroFields:
      return "zero-fields";
    case SchemaError::EmptyName:
      return "empty-name";
    case SchemaError::DuplicateFieldName:
      return "duplicate-field-name";
    case SchemaError::NameTooLong:
      return "name-too-long";
    case SchemaError::StrideTooLarge:
      return "stride-too-large";
    case SchemaError::TrailingBytes:
      return "trailing-bytes";
    case SchemaError::NotSchemaFrame:
      return "not-schema-frame";
  }
  return "unknown";
}

}
