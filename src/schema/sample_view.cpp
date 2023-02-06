#include <cstring>

#include "blackspan/schema.h"

namespace blackspan {

bool SampleView::read_raw(size_t field_index, FieldType expect,
                          uint64_t* raw) const {
  if (field_index >= schema_.fields.size()) return false;
  const SchemaField& f = schema_.fields[field_index];
  if (f.type != expect) return false;
  const size_t sz = field_type_size(f.type);
  if (sz == 0) return false;
  if (f.offset > len_ || sz > len_ - f.offset) return false;  // short span

  uint64_t v = 0;
  for (size_t i = 0; i < sz; ++i) {
    v |= static_cast<uint64_t>(data_[f.offset + i]) << (8 * i);
  }
  *raw = v;
  return true;
}

std::optional<uint8_t> SampleView::get_u8(size_t i) const {
  uint64_t raw;
  if (!read_raw(i, FieldType::U8, &raw)) return std::nullopt;
  return static_cast<uint8_t>(raw);
}

std::optional<int8_t> SampleView::get_i8(size_t i) const {
  uint64_t raw;
  if (!read_raw(i, FieldType::I8, &raw)) return std::nullopt;
  return static_cast<int8_t>(static_cast<uint8_t>(raw));
}

std::optional<uint16_t> SampleView::get_u16(size_t i) const {
  uint64_t raw;
  if (!read_raw(i, FieldType::U16, &raw)) return std::nullopt;
  return static_cast<uint16_t>(raw);
}

std::optional<int16_t> SampleView::get_i16(size_t i) const {
  uint64_t raw;
  if (!read_raw(i, FieldType::I16, &raw)) return std::nullopt;
  return static_cast<int16_t>(static_cast<uint16_t>(raw));
}

std::optional<uint32_t> SampleView::get_u32(size_t i) const {
  uint64_t raw;
  if (!read_raw(i, FieldType::U32, &raw)) return std::nullopt;
  return static_cast<uint32_t>(raw);
}

std::optional<int32_t> SampleView::get_i32(size_t i) const {
  uint64_t raw;
  if (!read_raw(i, FieldType::I32, &raw)) return std::nullopt;
  return static_cast<int32_t>(static_cast<uint32_t>(raw));
}

std::optional<uint64_t> SampleView::get_u64(size_t i) const {
  uint64_t raw;
  if (!read_raw(i, FieldType::U64, &raw)) return std::nullopt;
  return raw;
}

std::optional<int64_t> SampleView::get_i64(size_t i) const {
  uint64_t raw;
  if (!read_raw(i, FieldType::I64, &raw)) return std::nullopt;
  return static_cast<int64_t>(raw);
}

std::optional<float> SampleView::get_f32(size_t i) const {
  uint64_t raw;
  if (!read_raw(i, FieldType::F32, &raw)) return std::nullopt;
  const uint32_t bits = static_cast<uint32_t>(raw);
  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

std::optional<double> SampleView::get_f64(size_t i) const {
  uint64_t raw;
  if (!read_raw(i, FieldType::F64, &raw)) return std::nullopt;
  double out;
  std::memcpy(&out, &raw, sizeof(out));
  return out;
}

std::optional<double> SampleView::get_as_double(size_t i) const {
  if (i >= schema_.fields.size()) return std::nullopt;
  switch (schema_.fields[i].type) {
    case FieldType::U8: {
      const auto v = get_u8(i);
      return v ? std::optional<double>(static_cast<double>(*v)) : std::nullopt;
    }
    case FieldType::I8: {
      const auto v = get_i8(i);
      return v ? std::optional<double>(static_cast<double>(*v)) : std::nullopt;
    }
    case FieldType::U16: {
      const auto v = get_u16(i);
      return v ? std::optional<double>(static_cast<double>(*v)) : std::nullopt;
    }
    case FieldType::I16: {
      const auto v = get_i16(i);
      return v ? std::optional<double>(static_cast<double>(*v)) : std::nullopt;
    }
    case FieldType::U32: {
      const auto v = get_u32(i);
      return v ? std::optional<double>(static_cast<double>(*v)) : std::nullopt;
    }
    case FieldType::I32: {
      const auto v = get_i32(i);
      return v ? std::optional<double>(static_cast<double>(*v)) : std::nullopt;
    }
    case FieldType::U64: {
      const auto v = get_u64(i);
      return v ? std::optional<double>(static_cast<double>(*v)) : std::nullopt;
    }
    case FieldType::I64: {
      const auto v = get_i64(i);
      return v ? std::optional<double>(static_cast<double>(*v)) : std::nullopt;
    }
    case FieldType::F32: {
      const auto v = get_f32(i);
      return v ? std::optional<double>(static_cast<double>(*v)) : std::nullopt;
    }
    case FieldType::F64: {
      const auto v = get_f64(i);
      return v ? std::optional<double>(*v) : std::nullopt;
    }
  }
  return std::nullopt;
}

}  // namespace blackspan
