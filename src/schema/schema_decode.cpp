#include "blackspan/byteio.h"
#include "blackspan/schema.h"

namespace blackspan {

SchemaDecodeResult decode_channel_schema(const uint8_t* payload, size_t len) {
  SchemaDecodeResult res;
  ByteReader r(payload, len);

  const uint16_t channel_id = r.u16();
  const uint8_t declared_version = r.u8();
  const uint8_t field_count = r.u8();
  const uint8_t name_len = r.u8();
  if (!r.ok()) {
    res.error = SchemaError::Truncated;
    return res;
  }
  if (field_count == 0) {
    res.error = SchemaError::ZeroFields;
    return res;
  }
  if (name_len > kMaxSchemaNameLen) {
    res.error = SchemaError::NameTooLong;
    return res;
  }

  res.schema.channel_id = channel_id;
  res.declared_version = declared_version;
  if (name_len) {
    const uint8_t* p = r.take(name_len);
    if (!p) {
      res.error = SchemaError::Truncated;
      return res;
    }
    res.schema.name.assign(reinterpret_cast<const char*>(p), name_len);
  }

  res.schema.fields.reserve(field_count);
  for (size_t i = 0; i < field_count; ++i) {
    const uint8_t type_byte = r.u8();
    const uint8_t fn_len = r.u8();
    if (!r.ok()) {
      res.error = SchemaError::Truncated;
      return res;
    }
    const std::optional<FieldType> ft = parse_field_type(type_byte);
    if (!ft) {
      res.error = SchemaError::BadFieldType;
      return res;
    }
    if (fn_len == 0) {
      res.error = SchemaError::EmptyName;
      return res;
    }
    if (fn_len > kMaxSchemaNameLen) {
      res.error = SchemaError::NameTooLong;
      return res;
    }
    const uint8_t* p = r.take(fn_len);
    if (!p) {
      res.error = SchemaError::Truncated;
      return res;
    }
    SchemaField f;
    f.name.assign(reinterpret_cast<const char*>(p), fn_len);
    f.type = *ft;
    res.schema.fields.push_back(std::move(f));
  }

  if (r.remaining() != 0) {
    res.error = SchemaError::TrailingBytes;
    return res;
  }

  res.schema.recompute_layout();
  res.error = res.schema.validate();
  return res;
}

}
