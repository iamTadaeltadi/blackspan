#include "blackspan/derive.h"

#include <utility>

#include "blackspan/byteio.h"

namespace blackspan {

const char* derive_error_str(DeriveError e) {
  switch (e) {
    case DeriveError::None:
      return "ok";
    case DeriveError::NotDerivedDef:
      return "not-derived-def";
    case DeriveError::Truncated:
      return "truncated";
    case DeriveError::ParseFailed:
      return "parse-failed";
    case DeriveError::DuplicateId:
      return "duplicate-id";
    case DeriveError::UnknownId:
      return "unknown-id";
    case DeriveError::NoPrimaryChannel:
      return "no-primary-channel";
    case DeriveError::UnknownFieldRef:
      return "unknown-field-ref";
    case DeriveError::ChannelTooShort:
      return "channel-too-short";
  }
  return "unknown";
}

DeriveError decode_derived_def(const uint8_t* payload, size_t len,
                               DerivedDefRecord& out) {
  out = DerivedDefRecord{};
  ByteReader r(payload, len);
  out.derived_id = r.u16();
  out.primary_channel = r.u16();
  const uint8_t name_len = r.u8();
  if (!r.ok()) return DeriveError::Truncated;
  if (name_len > 0) {
    const uint8_t* np = r.take(name_len);
    if (np == nullptr) return DeriveError::Truncated;
    out.name.assign(reinterpret_cast<const char*>(np), name_len);
  }
  const uint16_t expr_len = r.u16();
  if (!r.ok()) return DeriveError::Truncated;
  if (expr_len > 0) {
    const uint8_t* ep = r.take(expr_len);
    if (ep == nullptr) return DeriveError::Truncated;
    out.expr.assign(reinterpret_cast<const char*>(ep), expr_len);
  }
  return DeriveError::None;
}

DeriveError DeriveEngine::register_def(const DerivedDefRecord& def,
                                       ExprError* parse_err, size_t* parse_pos) {
  if (parse_err) *parse_err = ExprError::None;
  if (parse_pos) *parse_pos = 0;
  if (defs_.find(def.derived_id) != defs_.end()) {
    return DeriveError::DuplicateId;
  }
  ParseResult pr = parse_expr(def.expr);
  if (!pr.ok()) {
    if (parse_err) *parse_err = pr.error;
    if (parse_pos) *parse_pos = pr.position;
    return DeriveError::ParseFailed;
  }
  Entry e;
  e.def = def;
  e.ast = std::move(pr.ast);
  defs_.emplace(def.derived_id, std::move(e));
  return DeriveError::None;
}

DeriveError DeriveEngine::ingest_frame(const Frame& f, ExprError* parse_err,
                                       size_t* parse_pos) {
  if (parse_err) *parse_err = ExprError::None;
  if (parse_pos) *parse_pos = 0;
  if (f.header.type != RecordType::DerivedDef) {
    return DeriveError::NotDerivedDef;
  }
  DerivedDefRecord rec;
  const DeriveError de =
      decode_derived_def(f.payload.data(), f.payload.size(), rec);
  if (de != DeriveError::None) return de;
  return register_def(rec, parse_err, parse_pos);
}

DeriveError DeriveEngine::evaluate(uint16_t derived_id,
                                   const ChannelStore& store,
                                   DerivedSeries& out) const {
  out = DerivedSeries{};
  const auto it = defs_.find(derived_id);
  if (it == defs_.end()) return DeriveError::UnknownId;
  const Entry& entry = it->second;

  const ChannelSeries* primary = store.series(entry.def.primary_channel);
  if (primary == nullptr) return DeriveError::NoPrimaryChannel;

  const size_t n = primary->total_samples();

  // Each primary sample inherits the base timestamp of the block that carries
  // it; expand the per-block bases into a per-sample timeline once up front.
  std::vector<uint64_t> timestamps;
  timestamps.reserve(n);
  for (size_t b = 0; b < primary->block_count(); ++b) {
    const uint16_t count = primary->block_sample_count(b);
    const uint64_t base = primary->block_base_ts(b);
    for (uint16_t k = 0; k < count; ++k) timestamps.push_back(base);
  }

  std::vector<double> values;
  values.reserve(n);

  for (size_t i = 0; i < n; ++i) {
    // Resolve every field reference against the sample at the same global index
    // on its channel, latching a specific error for the caller.
    DeriveError bind_err = DeriveError::None;
    const ExprBinding bind =
        [&](const FieldReference& ref) -> std::optional<double> {
      const ChannelSeries* src =
          ref.cross ? store.series(ref.channel_id) : primary;
      if (src == nullptr) {
        bind_err = DeriveError::UnknownFieldRef;
        return std::nullopt;
      }
      const std::optional<size_t> fidx = src->schema().field_index(ref.name);
      if (!fidx) {
        bind_err = DeriveError::UnknownFieldRef;
        return std::nullopt;
      }
      const std::optional<SampleView> sv = src->view(i);
      if (!sv) {
        bind_err = DeriveError::ChannelTooShort;
        return std::nullopt;
      }
      const std::optional<double> d = sv->get_as_double(*fidx);
      if (!d) {
        bind_err = DeriveError::UnknownFieldRef;
        return std::nullopt;
      }
      return *d;
    };

    double value = 0.0;
    if (!eval_expr(*entry.ast, bind, &value, nullptr)) {
      out = DerivedSeries{};
      return bind_err != DeriveError::None ? bind_err
                                           : DeriveError::UnknownFieldRef;
    }
    values.push_back(value);
  }

  out.derived_id = derived_id;
  out.values = std::move(values);
  out.timestamps = std::move(timestamps);
  return DeriveError::None;
}

std::vector<uint16_t> DeriveEngine::defs() const {
  std::vector<uint16_t> out;
  out.reserve(defs_.size());
  for (const auto& kv : defs_) out.push_back(kv.first);
  return out;  // std::map iterates keys ascending
}

const DerivedDefRecord* DeriveEngine::get(uint16_t derived_id) const {
  const auto it = defs_.find(derived_id);
  if (it == defs_.end()) return nullptr;
  return &it->second.def;
}

}  // namespace blackspan
