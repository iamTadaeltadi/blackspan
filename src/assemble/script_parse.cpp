#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

#include "assemble_internal.h"
#include "blackspan/assemble.h"
#include "blackspan/frame.h"
#include "blackspan/schema.h"

namespace blackspan {
namespace {

using assemble_detail::DamageMode;

std::vector<std::string> split_commas(const std::string& s) {
  std::vector<std::string> parts;
  if (s.empty()) return parts;
  size_t start = 0;
  while (true) {
    size_t comma = s.find(',', start);
    if (comma == std::string::npos) {
      parts.push_back(s.substr(start));
      break;
    }
    parts.push_back(s.substr(start, comma - start));
    start = comma + 1;
  }
  return parts;
}

bool split_field_spec(const std::string& s, std::string& name,
                      std::string& type) {
  size_t colon = s.find(':');
  if (colon == std::string::npos) return false;
  name = s.substr(0, colon);
  type = s.substr(colon + 1);
  return !name.empty() && !type.empty();
}

struct HeaderState {
  uint32_t device_id = 0;
  uint32_t session_seq = 0;
  uint64_t monotonic_ts = 0;
  uint16_t flags = 0;
  uint64_t tick = 0;
};

class Assembler {
 public:
  void feed(const std::string& text, size_t lineno);
  void finish();

  AssembleResult take() {
    AssembleResult r;
    r.frame_count = frame_count_;
    r.errors = std::move(errors_);
    if (r.errors.empty()) r.bytes = std::move(bytes_);
    return r;
  }

 private:

  void fail(AssembleError e, const std::string& detail) {
    AssembleDiagnostic d;
    d.line = line_no_;
    d.error = e;
    d.detail = detail;
    errors_.push_back(std::move(d));
  }

  bool check_keys(const ScriptLine& line,
                  std::initializer_list<const char*> allowed);

  const ScriptArg* positional(const ScriptLine& line, size_t index) const;
  bool req_uint(const ScriptLine& l, const char* key, uint64_t max,
                uint64_t& out);
  bool opt_uint(const ScriptLine& l, const char* key, uint64_t max,
                uint64_t& out);
  bool state_uint(const ScriptLine& l, const char* key, uint64_t max,
                  uint64_t& out);
  bool opt_text(const ScriptLine& l, const char* key, std::string& out);

  void do_device(const ScriptLine& l);
  void do_session(const ScriptLine& l);
  void do_ts(const ScriptLine& l);
  void do_advance(const ScriptLine& l);
  void do_tick(const ScriptLine& l);
  void do_flags(const ScriptLine& l);
  void do_session_begin(const ScriptLine& l);
  void do_session_end(const ScriptLine& l);
  void do_schema(const ScriptLine& l);
  void do_samples(const ScriptLine& l);
  void do_row(const ScriptLine& l);
  void do_end(const ScriptLine& l);
  void do_state(const ScriptLine& l);
  void do_config(const ScriptLine& l);
  void do_fault(const ScriptLine& l);
  void do_marker(const ScriptLine& l);
  void do_checkpoint(const ScriptLine& l);
  void do_reconnect(const ScriptLine& l);
  void do_derived(const ScriptLine& l);
  void do_export(const ScriptLine& l);
  void do_raw(const ScriptLine& l);
  void do_damage(const ScriptLine& l);

  void emit(const ScriptLine& l, RecordType type,
            const std::vector<uint8_t>& payload);

  HeaderState st_;
  std::vector<uint8_t> bytes_;
  std::vector<AssembleDiagnostic> errors_;
  size_t frame_count_ = 0;
  size_t line_no_ = 0;

  size_t last_frame_start_ = 0;
  bool have_frame_ = false;

  ScriptSchemas schemas_;

  bool in_block_ = false;
  size_t block_line_ = 0;
  const ChannelSchema* block_schema_ = nullptr;
  uint16_t block_channel_ = 0;
  uint16_t block_version_ = 0;
  uint32_t block_delta_ = 0;
  uint16_t block_flags_ = 0;
  size_t block_rows_ = 0;
  std::vector<uint8_t> block_bytes_;
  ScriptLine block_line_args_;
};

bool Assembler::check_keys(const ScriptLine& line,
                           std::initializer_list<const char*> allowed) {
  std::set<std::string> seen;
  for (const ScriptArg& a : line.args) {
    if (a.key.empty()) continue;
    bool known = false;
    for (const char* k : allowed) {
      if (a.key == k) {
        known = true;
        break;
      }
    }
    if (!known) {
      fail(AssembleError::UnknownArgument, a.key);
      return false;
    }
    if (!seen.insert(a.key).second) {
      fail(AssembleError::DuplicateArgument, a.key);
      return false;
    }
  }
  return true;
}

const ScriptArg* Assembler::positional(const ScriptLine& line,
                                       size_t index) const {
  size_t n = 0;
  for (const ScriptArg& a : line.args) {
    if (!a.key.empty()) continue;
    if (n == index) return &a;
    ++n;
  }
  return nullptr;
}

bool Assembler::req_uint(const ScriptLine& l, const char* key, uint64_t max,
                         uint64_t& out) {
  const ScriptArg* a = find_arg(l, key);
  if (!a) {
    fail(AssembleError::MissingArgument, key);
    return false;
  }
  if (!parse_uint(a->value, UINT64_MAX, out)) {
    fail(AssembleError::BadArgument, std::string(key) + "=" + a->value);
    return false;
  }
  if (out > max) {
    fail(AssembleError::ValueOutOfRange, std::string(key) + "=" + a->value);
    return false;
  }
  return true;
}

bool Assembler::opt_uint(const ScriptLine& l, const char* key, uint64_t max,
                         uint64_t& out) {
  const ScriptArg* a = find_arg(l, key);
  if (!a) return true;
  if (!parse_uint(a->value, UINT64_MAX, out)) {
    fail(AssembleError::BadArgument, std::string(key) + "=" + a->value);
    return false;
  }
  if (out > max) {
    fail(AssembleError::ValueOutOfRange, std::string(key) + "=" + a->value);
    return false;
  }
  return true;
}

bool Assembler::state_uint(const ScriptLine& l, const char* key, uint64_t max,
                           uint64_t& out) {
  const ScriptArg* a = find_arg(l, key);
  if (!a) a = positional(l, 0);
  if (!a) {
    fail(AssembleError::MissingArgument, key);
    return false;
  }
  if (!parse_uint(a->value, UINT64_MAX, out)) {
    fail(AssembleError::BadArgument, std::string(key) + "=" + a->value);
    return false;
  }
  if (out > max) {
    fail(AssembleError::ValueOutOfRange, std::string(key) + "=" + a->value);
    return false;
  }
  return true;
}

bool Assembler::opt_text(const ScriptLine& l, const char* key,
                         std::string& out) {
  const ScriptArg* a = find_arg(l, key);
  if (!a) return true;
  out = a->value;
  return true;
}

void Assembler::emit(const ScriptLine& l, RecordType type,
                     const std::vector<uint8_t>& payload) {
  if (payload.size() > kMaxPayload) {
    fail(AssembleError::PayloadTooLong,
         std::to_string(payload.size()) + " bytes");
    return;
  }

  uint64_t dev = st_.device_id;
  uint64_t seq = st_.session_seq;
  uint64_t ts = st_.monotonic_ts;
  uint64_t flags = st_.flags;
  if (!opt_uint(l, "dev", UINT32_MAX, dev)) return;
  if (!opt_uint(l, "seq", UINT32_MAX, seq)) return;
  if (!opt_uint(l, "ts", UINT64_MAX, ts)) return;
  if (!opt_uint(l, "flags", UINT16_MAX, flags)) return;

  FrameHeader h;
  h.version = kFrameVersion;
  h.type = type;
  h.flags = static_cast<uint16_t>(flags);
  h.device_id = static_cast<uint32_t>(dev);
  h.session_seq = static_cast<uint32_t>(seq);
  h.monotonic_ts = ts;
  h.payload_len = static_cast<uint16_t>(payload.size());

  last_frame_start_ = bytes_.size();
  have_frame_ = true;
  encode_frame(h, payload.data(), payload.size(), bytes_);
  ++frame_count_;

  st_.session_seq += 1;
  st_.monotonic_ts += st_.tick;
}

void Assembler::do_device(const ScriptLine& l) {
  if (!check_keys(l, {"id"})) return;
  uint64_t v = 0;
  if (!state_uint(l, "id", UINT32_MAX, v)) return;
  st_.device_id = static_cast<uint32_t>(v);
}

void Assembler::do_session(const ScriptLine& l) {
  if (!check_keys(l, {"seq"})) return;
  uint64_t v = 0;
  if (!state_uint(l, "seq", UINT32_MAX, v)) return;
  st_.session_seq = static_cast<uint32_t>(v);
}

void Assembler::do_ts(const ScriptLine& l) {
  if (!check_keys(l, {"value"})) return;
  uint64_t v = 0;
  if (!state_uint(l, "value", UINT64_MAX, v)) return;
  st_.monotonic_ts = v;
}

void Assembler::do_advance(const ScriptLine& l) {
  if (!check_keys(l, {"by"})) return;
  uint64_t v = 0;
  if (!state_uint(l, "by", UINT64_MAX, v)) return;
  st_.monotonic_ts += v;
}

void Assembler::do_tick(const ScriptLine& l) {
  if (!check_keys(l, {"by"})) return;
  uint64_t v = 0;
  if (!state_uint(l, "by", UINT64_MAX, v)) return;
  st_.tick = v;
}

void Assembler::do_flags(const ScriptLine& l) {
  if (!check_keys(l, {"value"})) return;
  uint64_t v = 0;
  if (!state_uint(l, "value", UINT16_MAX, v)) return;
  st_.flags = static_cast<uint16_t>(v);
}

void Assembler::do_session_begin(const ScriptLine& l) {
  if (!check_keys(l, {"class", "fw", "epoch", "label", "dev", "seq", "ts",
                      "flags"})) {
    return;
  }
  SessionBegin r;
  uint64_t cls = 0, epoch = 0;
  if (!opt_uint(l, "class", UINT16_MAX, cls)) return;
  if (!opt_uint(l, "epoch", UINT32_MAX, epoch)) return;
  r.device_class = static_cast<uint16_t>(cls);
  r.schema_epoch = static_cast<uint32_t>(epoch);

  const ScriptArg* fw = find_arg(l, "fw");
  if (fw) {

    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= fw->value.size(); ++i) {
      if (i == fw->value.size() || fw->value[i] == '.') {
        parts.push_back(fw->value.substr(start, i - start));
        start = i + 1;
      }
    }
    if (parts.size() != 3) {
      fail(AssembleError::BadArgument, "fw=" + fw->value);
      return;
    }
    uint64_t maj = 0, min = 0, pat = 0;
    if (!parse_uint(parts[0], UINT8_MAX, maj) ||
        !parse_uint(parts[1], UINT8_MAX, min) ||
        !parse_uint(parts[2], UINT16_MAX, pat)) {
      fail(AssembleError::BadArgument, "fw=" + fw->value);
      return;
    }
    r.fw_major = static_cast<uint8_t>(maj);
    r.fw_minor = static_cast<uint8_t>(min);
    r.fw_patch = static_cast<uint16_t>(pat);
  }
  if (!opt_text(l, "label", r.label)) return;

  std::vector<uint8_t> payload;
  assemble_detail::write_session_begin(r, payload);
  emit(l, RecordType::SessionBegin, payload);
}

void Assembler::do_session_end(const ScriptLine& l) {
  if (!check_keys(l, {"reason", "dev", "seq", "ts", "flags"})) return;
  SessionEnd r;
  uint64_t reason = 0;
  if (!opt_uint(l, "reason", UINT16_MAX, reason)) return;
  r.reason_code = static_cast<uint16_t>(reason);
  std::vector<uint8_t> payload;
  assemble_detail::write_session_end(r, payload);
  emit(l, RecordType::SessionEnd, payload);
}

void Assembler::do_schema(const ScriptLine& l) {
  if (!check_keys(l, {"id", "name", "fields", "dev", "seq", "ts", "flags"})) {
    return;
  }
  uint64_t id = 0;
  if (!req_uint(l, "id", UINT16_MAX, id)) return;

  const ScriptArg* fields = find_arg(l, "fields");
  if (!fields) {
    fail(AssembleError::MissingArgument, "fields");
    return;
  }
  std::vector<std::string> specs = split_commas(fields->value);
  if (specs.empty()) {
    fail(AssembleError::EmptySchema, "fields");
    return;
  }

  ChannelSchema s;
  s.channel_id = static_cast<uint16_t>(id);
  if (!opt_text(l, "name", s.name)) return;
  for (const std::string& spec : specs) {
    std::string fname, ftype;
    if (!split_field_spec(spec, fname, ftype)) {
      fail(AssembleError::BadArgument, "fields=" + spec);
      return;
    }
    std::optional<FieldType> t = parse_field_type_name(ftype);
    if (!t) {
      fail(AssembleError::UnknownFieldType, ftype);
      return;
    }
    for (const SchemaField& existing : s.fields) {
      if (existing.name == fname) {
        fail(AssembleError::DuplicateFieldName, fname);
        return;
      }
    }
    SchemaField f;
    f.name = fname;
    f.type = *t;
    s.fields.push_back(f);
  }
  s.recompute_layout();

  SchemaError verr = s.validate();
  if (verr != SchemaError::None) {
    fail(AssembleError::BadArgument, schema_error_str(verr));
    return;
  }

  uint16_t version = schemas_.declare(s);
  std::vector<uint8_t> payload;
  encode_schema_payload(s, version, payload);
  emit(l, RecordType::SchemaDef, payload);
}

void Assembler::do_samples(const ScriptLine& l) {
  if (!check_keys(l, {"channel", "version", "delta", "blockflags", "dev", "seq",
                      "ts", "flags"})) {
    return;
  }
  uint64_t channel = 0;
  if (!req_uint(l, "channel", UINT16_MAX, channel)) return;

  uint64_t version = schemas_.latest_version(static_cast<uint16_t>(channel));
  if (version == 0) {
    fail(AssembleError::UnknownChannel, "channel=" + std::to_string(channel));
    return;
  }
  if (!opt_uint(l, "version", UINT16_MAX, version)) return;

  const ChannelSchema* schema = schemas_.get(static_cast<uint16_t>(channel),
                                             static_cast<uint16_t>(version));
  if (!schema) {
    fail(AssembleError::UnknownVersion, "version=" + std::to_string(version));
    return;
  }

  uint64_t delta = 0, blockflags = 0;
  if (!opt_uint(l, "delta", UINT32_MAX, delta)) return;
  if (!opt_uint(l, "blockflags", UINT16_MAX, blockflags)) return;

  in_block_ = true;
  block_line_ = line_no_;
  block_schema_ = schema;
  block_channel_ = static_cast<uint16_t>(channel);
  block_version_ = static_cast<uint16_t>(version);
  block_delta_ = static_cast<uint32_t>(delta);
  block_flags_ = static_cast<uint16_t>(blockflags);
  block_rows_ = 0;
  block_bytes_.clear();
  block_line_args_ = l;
}

void Assembler::do_row(const ScriptLine& l) {
  if (!in_block_) {
    fail(AssembleError::RowOutsideBlock, "row");
    return;
  }
  if (!check_keys(l, {})) return;

  std::vector<std::string> values;
  for (const ScriptArg& a : l.args) {
    if (a.key.empty()) values.push_back(a.value);
  }
  if (values.size() != block_schema_->fields.size()) {
    fail(AssembleError::RowFieldCount,
         std::to_string(values.size()) + " of " +
             std::to_string(block_schema_->fields.size()));
    return;
  }
  size_t bad = 0;
  if (!encode_sample_row(*block_schema_, values, block_bytes_, &bad)) {
    fail(AssembleError::BadArgument,
         block_schema_->fields[bad].name + "=" + values[bad]);
    return;
  }
  ++block_rows_;
}

void Assembler::do_end(const ScriptLine& l) {
  if (!in_block_) {
    fail(AssembleError::StrayEnd, "end");
    return;
  }
  if (!check_keys(l, {})) return;

  in_block_ = false;
  if (block_rows_ == 0) {
    fail(AssembleError::EmptyBlock, "channel=" + std::to_string(block_channel_));
    return;
  }
  if (block_rows_ > UINT16_MAX) {
    fail(AssembleError::ValueOutOfRange,
         "rows=" + std::to_string(block_rows_));
    return;
  }

  std::vector<uint8_t> payload;
  assemble_detail::write_sample_block_header(
      block_channel_, block_version_, block_delta_,
      static_cast<uint16_t>(block_rows_), block_flags_, payload);
  payload.insert(payload.end(), block_bytes_.begin(), block_bytes_.end());

  emit(block_line_args_, RecordType::SampleBlock, payload);
  block_bytes_.clear();
}

void Assembler::do_state(const ScriptLine& l) {
  if (!check_keys(l, {"from", "to", "cause", "reserved", "dev", "seq", "ts",
                      "flags"})) {
    return;
  }
  StateTransition r;
  uint64_t from = 0, to = 0, cause = 0, reserved = 0;
  if (!req_uint(l, "from", UINT16_MAX, from)) return;
  if (!req_uint(l, "to", UINT16_MAX, to)) return;
  if (!opt_uint(l, "cause", UINT8_MAX, cause)) return;
  if (!opt_uint(l, "reserved", UINT8_MAX, reserved)) return;
  r.from_state = static_cast<uint16_t>(from);
  r.to_state = static_cast<uint16_t>(to);
  r.cause = static_cast<uint8_t>(cause);
  r.reserved = static_cast<uint8_t>(reserved);
  std::vector<uint8_t> payload;
  assemble_detail::write_state_transition(r, payload);
  emit(l, RecordType::StateTransition, payload);
}

void Assembler::do_config(const ScriptLine& l) {
  if (!check_keys(l, {"param", "old", "new", "dev", "seq", "ts", "flags"})) {
    return;
  }
  ConfigChange r;
  uint64_t param = 0, old_value = 0, new_value = 0;
  if (!req_uint(l, "param", UINT16_MAX, param)) return;
  if (!opt_uint(l, "old", UINT32_MAX, old_value)) return;
  if (!opt_uint(l, "new", UINT32_MAX, new_value)) return;
  r.param_id = static_cast<uint16_t>(param);
  r.old_value = static_cast<uint32_t>(old_value);
  r.new_value = static_cast<uint32_t>(new_value);
  std::vector<uint8_t> payload;
  assemble_detail::write_config_change(r, payload);
  emit(l, RecordType::ConfigChange, payload);
}

void Assembler::do_fault(const ScriptLine& l) {
  if (!check_keys(l, {"code", "severity", "subsystem", "detail", "dev", "seq",
                      "ts", "flags"})) {
    return;
  }
  Fault r;
  uint64_t code = 0, subsystem = 0;
  if (!req_uint(l, "code", UINT16_MAX, code)) return;
  if (!opt_uint(l, "subsystem", UINT8_MAX, subsystem)) return;
  r.fault_code = static_cast<uint16_t>(code);
  r.subsystem_id = static_cast<uint8_t>(subsystem);

  const ScriptArg* sev = find_arg(l, "severity");
  if (sev) {
    std::optional<uint8_t> s = parse_severity_name(sev->value);
    if (!s) {
      fail(AssembleError::BadArgument, "severity=" + sev->value);
      return;
    }
    r.severity = severity_from_byte(*s);
  }
  const ScriptArg* detail = find_arg(l, "detail");
  if (detail) {
    r.detail.assign(detail->value.begin(), detail->value.end());
    if (r.detail.size() > 255) {
      fail(AssembleError::ValueOutOfRange,
           "detail=" + std::to_string(r.detail.size()) + " bytes");
      return;
    }
  }
  std::vector<uint8_t> payload;
  assemble_detail::write_fault(r, payload);
  emit(l, RecordType::Fault, payload);
}

void Assembler::do_marker(const ScriptLine& l) {
  if (!check_keys(l, {"kind", "text", "dev", "seq", "ts", "flags"})) return;
  Marker r;
  uint64_t kind = 0;
  if (!opt_uint(l, "kind", UINT8_MAX, kind)) return;
  r.marker_kind = static_cast<uint8_t>(kind);
  if (!opt_text(l, "text", r.text)) return;
  if (r.text.size() > 255) {
    fail(AssembleError::ValueOutOfRange,
         "text=" + std::to_string(r.text.size()) + " bytes");
    return;
  }
  std::vector<uint8_t> payload;
  assemble_detail::write_marker(r, payload);
  emit(l, RecordType::Marker, payload);
}

void Assembler::do_checkpoint(const ScriptLine& l) {
  if (!check_keys(l, {"id", "bytes", "dev", "seq", "ts", "flags"})) return;
  Checkpoint r;
  uint64_t id = 0, since = 0;
  if (!opt_uint(l, "id", UINT32_MAX, id)) return;
  if (!opt_uint(l, "bytes", UINT32_MAX, since)) return;
  r.checkpoint_id = static_cast<uint32_t>(id);
  r.bytes_since_last = static_cast<uint32_t>(since);
  std::vector<uint8_t> payload;
  assemble_detail::write_checkpoint(r, payload);
  emit(l, RecordType::Checkpoint, payload);
}

void Assembler::do_reconnect(const ScriptLine& l) {
  if (!check_keys(l, {"gap", "prev", "dev", "seq", "ts", "flags"})) return;
  Reconnect r;
  uint64_t gap = 0, prev = 0;
  if (!opt_uint(l, "gap", UINT32_MAX, gap)) return;
  if (!opt_uint(l, "prev", UINT32_MAX, prev)) return;
  r.gap_ms = static_cast<uint32_t>(gap);
  r.prev_session_seq = static_cast<uint32_t>(prev);
  std::vector<uint8_t> payload;
  assemble_detail::write_reconnect(r, payload);
  emit(l, RecordType::Reconnect, payload);
}

void Assembler::do_derived(const ScriptLine& l) {
  if (!check_keys(l, {"id", "primary", "name", "expr", "dev", "seq", "ts",
                      "flags"})) {
    return;
  }
  DerivedDefRecord r;
  uint64_t id = 0, primary = 0;
  if (!req_uint(l, "id", UINT16_MAX, id)) return;
  if (!req_uint(l, "primary", UINT16_MAX, primary)) return;
  r.derived_id = static_cast<uint16_t>(id);
  r.primary_channel = static_cast<uint16_t>(primary);
  if (!opt_text(l, "name", r.name)) return;
  const ScriptArg* expr = find_arg(l, "expr");
  if (!expr) {
    fail(AssembleError::MissingArgument, "expr");
    return;
  }
  r.expr = expr->value;
  std::vector<uint8_t> payload;
  assemble_detail::write_derived_def(r, payload);
  emit(l, RecordType::DerivedDef, payload);
}

void Assembler::do_export(const ScriptLine& l) {
  if (!check_keys(l, {"format", "sections", "dev", "seq", "ts", "flags"})) {
    return;
  }
  ExportRequest r;
  const ScriptArg* fmt = find_arg(l, "format");
  if (fmt) {
    if (fmt->value == "text")
      r.format = ReportFormat::Text;
    else if (fmt->value == "json")
      r.format = ReportFormat::Json;
    else if (fmt->value == "dot")
      r.format = ReportFormat::Dot;
    else {
      fail(AssembleError::BadArgument, "format=" + fmt->value);
      return;
    }
  }
  uint64_t sections = 0;
  if (!opt_uint(l, "sections", UINT16_MAX, sections)) return;
  r.section_mask = static_cast<uint16_t>(sections);
  std::vector<uint8_t> payload;
  assemble_detail::write_export_request(r, payload);
  emit(l, RecordType::ExportRequest, payload);
}

void Assembler::do_raw(const ScriptLine& l) {
  if (!check_keys(l, {"type", "hex", "text", "dev", "seq", "ts", "flags"})) {
    return;
  }
  uint64_t type = 0;
  if (!req_uint(l, "type", UINT8_MAX, type)) return;

  std::vector<uint8_t> payload;
  const ScriptArg* hex = find_arg(l, "hex");
  if (hex) {
    if (!parse_hex_bytes(hex->value, payload)) {
      fail(AssembleError::BadHex, "hex=" + hex->value);
      return;
    }
  }
  const ScriptArg* text = find_arg(l, "text");
  if (text) {
    payload.insert(payload.end(), text->value.begin(), text->value.end());
  }
  emit(l, static_cast<RecordType>(type), payload);
}

void Assembler::do_damage(const ScriptLine& l) {
  if (!check_keys(l, {"mode", "bytes"})) return;
  if (!have_frame_) {
    fail(AssembleError::NothingToDamage, "damage");
    return;
  }
  const ScriptArg* mode = find_arg(l, "mode");
  if (!mode) mode = positional(l, 0);
  if (!mode) {
    fail(AssembleError::MissingArgument, "mode");
    return;
  }
  DamageMode m;
  if (!assemble_detail::parse_damage_mode(mode->value, m)) {
    fail(AssembleError::BadDamageMode, mode->value);
    return;
  }
  uint64_t amount = 1;
  if (!opt_uint(l, "bytes", UINT32_MAX, amount)) return;
  if (!assemble_detail::apply_damage(bytes_, last_frame_start_, m, amount)) {
    fail(AssembleError::BadDamageMode, mode->value + " (frame too short)");
    return;
  }
}

void Assembler::feed(const std::string& text, size_t lineno) {
  line_no_ = lineno;

  ScriptLine line;
  line.line = lineno;
  AssembleError lex_err = AssembleError::None;
  if (!lex_script_line(text, line, &lex_err)) return;
  if (lex_err != AssembleError::None) {
    fail(lex_err, line.name);
    return;
  }

  const std::string& d = line.name;

  if (in_block_ && d != "row" && d != "end") {
    fail(d == "samples" ? AssembleError::BlockInsideBlock
                        : AssembleError::DirectiveInsideBlock,
         d);
    return;
  }

  if (d == "device") return do_device(line);
  if (d == "session") return do_session(line);
  if (d == "ts") return do_ts(line);
  if (d == "advance") return do_advance(line);
  if (d == "tick") return do_tick(line);
  if (d == "flags") return do_flags(line);
  if (d == "session-begin") return do_session_begin(line);
  if (d == "session-end") return do_session_end(line);
  if (d == "schema") return do_schema(line);
  if (d == "samples") return do_samples(line);
  if (d == "row") return do_row(line);
  if (d == "end") return do_end(line);
  if (d == "state") return do_state(line);
  if (d == "config") return do_config(line);
  if (d == "fault") return do_fault(line);
  if (d == "marker") return do_marker(line);
  if (d == "checkpoint") return do_checkpoint(line);
  if (d == "reconnect") return do_reconnect(line);
  if (d == "derived") return do_derived(line);
  if (d == "export") return do_export(line);
  if (d == "raw") return do_raw(line);
  if (d == "damage") return do_damage(line);

  fail(AssembleError::UnknownDirective, d);
}

void Assembler::finish() {
  if (in_block_) {
    line_no_ = block_line_;
    fail(AssembleError::UnterminatedBlock,
         "channel=" + std::to_string(block_channel_));
    in_block_ = false;
  }
}

}

AssembleResult assemble_script(const std::string& text) {
  Assembler a;
  size_t lineno = 1;
  size_t start = 0;
  while (start <= text.size()) {
    size_t nl = text.find('\n', start);
    size_t end = nl == std::string::npos ? text.size() : nl;
    a.feed(text.substr(start, end - start), lineno);
    ++lineno;
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  a.finish();
  return a.take();
}

}
