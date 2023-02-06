#include "blackspan/schema.h"

namespace blackspan {

void ChannelSchema::recompute_layout() {
  size_t off = 0;
  for (auto& f : fields) {
    f.offset = off;
    off += field_type_size(f.type);
  }
  stride = off;
}

SchemaError ChannelSchema::validate(size_t max_stride) const {
  if (fields.empty()) return SchemaError::ZeroFields;
  if (name.size() > kMaxSchemaNameLen) return SchemaError::NameTooLong;

  size_t total = 0;
  for (size_t i = 0; i < fields.size(); ++i) {
    const SchemaField& f = fields[i];
    if (f.name.empty()) return SchemaError::EmptyName;
    if (f.name.size() > kMaxSchemaNameLen) return SchemaError::NameTooLong;
    const size_t sz = field_type_size(f.type);
    if (sz == 0) return SchemaError::BadFieldType;
    for (size_t j = 0; j < i; ++j) {
      if (fields[j].name == f.name) return SchemaError::DuplicateFieldName;
    }
    total += sz;
  }
  if (total == 0 || total > max_stride) return SchemaError::StrideTooLarge;
  return SchemaError::None;
}

const SchemaField* ChannelSchema::field_by_name(
    const std::string& field_name) const {
  for (const auto& f : fields) {
    if (f.name == field_name) return &f;
  }
  return nullptr;
}

std::optional<size_t> ChannelSchema::field_index(
    const std::string& field_name) const {
  for (size_t i = 0; i < fields.size(); ++i) {
    if (fields[i].name == field_name) return i;
  }
  return std::nullopt;
}

bool ChannelSchema::same_layout(const ChannelSchema& other) const {
  if (name != other.name) return false;
  if (fields.size() != other.fields.size()) return false;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (fields[i].type != other.fields[i].type) return false;
    if (fields[i].name != other.fields[i].name) return false;
  }
  return true;
}

std::string ChannelSchema::describe() const {
  std::string out = "channel " + std::to_string(channel_id) + " v" +
                    std::to_string(version) + " \"" + name + "\" (stride " +
                    std::to_string(stride) + ", " +
                    std::to_string(fields.size()) + " fields):";
  for (size_t i = 0; i < fields.size(); ++i) {
    const SchemaField& f = fields[i];
    out += (i == 0 ? " " : ", ");
    out += f.name;
    out += ':';
    out += field_type_name(f.type);
    out += '@';
    out += std::to_string(f.offset);
  }
  return out;
}

}  // namespace blackspan
