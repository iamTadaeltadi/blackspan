#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>

#include "blackspan/query.h"

namespace blackspan {

namespace {

constexpr int kSampleIndex = -2;
constexpr int kSampleTs = -3;

bool apply_num(CompareOp op, double a, double b) {
  switch (op) {
    case CompareOp::Eq:
      return a == b;
    case CompareOp::Ne:
      return a != b;
    case CompareOp::Lt:
      return a < b;
    case CompareOp::Le:
      return a <= b;
    case CompareOp::Gt:
      return a > b;
    case CompareOp::Ge:
      return a >= b;
  }
  return false;
}

bool apply_str(CompareOp op, const std::string& a, const std::string& b) {
  switch (op) {
    case CompareOp::Eq:
      return a == b;
    case CompareOp::Ne:
      return a != b;
    case CompareOp::Lt:
      return a < b;
    case CompareOp::Le:
      return a <= b;
    case CompareOp::Gt:
      return a > b;
    case CompareOp::Ge:
      return a >= b;
  }
  return false;
}

bool eval_pred(const PredNode* n,
               const std::function<bool(const PredNode&)>& leaf) {
  if (n == nullptr) return true;
  switch (n->kind) {
    case PredKind::Comparison:
      return leaf(*n);
    case PredKind::Not:
      return !eval_pred(n->children[0].get(), leaf);
    case PredKind::And:
      for (const auto& c : n->children) {
        if (!eval_pred(c.get(), leaf)) return false;
      }
      return true;
    case PredKind::Or:
      for (const auto& c : n->children) {
        if (eval_pred(c.get(), leaf)) return true;
      }
      return false;
  }
  return false;
}

std::string fmt_double(double v) {
  if (std::isnan(v)) return "nan";
  if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
  char buf[64];
  if (std::floor(v) == v && std::fabs(v) < 9.2e18) {
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
  } else {
    std::snprintf(buf, sizeof(buf), "%.12g", v);
  }
  return std::string(buf);
}

size_t limit_of(const ParsedQuery& q) {
  return q.has_limit ? q.limit : std::numeric_limits<size_t>::max();
}

}

QueryEngine::QueryEngine(const std::vector<Session>* sessions,
                         const ChannelStore* channels,
                         const AnomalyEngine* anomalies,
                         const SchemaRegistry* schemas)
    : sessions_(sessions),
      channels_(channels),
      anomalies_(anomalies),
      schemas_(schemas) {}

QueryError QueryEngine::run(const std::string& query, QueryResult& out) const {
  out.entity.clear();
  out.rows.clear();

  ParsedQuery q;
  size_t pos = 0;
  const QueryError perr = parse_query(query, q, &pos);
  if (perr != QueryError::None) return perr;

  out.entity = query_entity_str(q.entity);
  switch (q.entity) {
    case QueryEntity::Faults:
      return run_faults(q, out);
    case QueryEntity::Transitions:
      return run_transitions(q, out);
    case QueryEntity::Sessions:
      return run_sessions(q, out);
    case QueryEntity::Channels:
      return run_channels(q, out);
    case QueryEntity::Samples:
      return run_samples(q, out);
  }
  return QueryError::None;
}

QueryError QueryEngine::run_faults(const ParsedQuery& q,
                                   QueryResult& out) const {
  if (anomalies_ == nullptr) return QueryError::None;
  const size_t limit = limit_of(q);

  for (const FaultEvent& f : anomalies_->faults()) {
    if (out.rows.size() >= limit) break;
    auto leaf = [&](const PredNode& n) -> bool {
      double lhs = 0.0;
      if (n.attr == "severity") {
        lhs = static_cast<double>(f.severity);
      } else if (n.attr == "code") {
        lhs = static_cast<double>(f.fault_code);
      } else if (n.attr == "subsystem") {
        lhs = static_cast<double>(f.subsystem_id);
      } else if (n.attr == "device") {
        lhs = static_cast<double>(f.device_id);
      } else if (n.attr == "seq") {
        lhs = static_cast<double>(f.session_seq);
      } else if (n.attr == "ts") {
        lhs = static_cast<double>(f.monotonic_ts);
      } else {
        return false;
      }
      return apply_num(n.op, lhs, n.value.number);
    };
    if (!eval_pred(q.predicate.get(), leaf)) continue;

    QueryResultRow row;
    row.columns = {
        {"device", std::to_string(f.device_id)},
        {"seq", std::to_string(f.session_seq)},
        {"ts", std::to_string(f.monotonic_ts)},
        {"code", std::to_string(f.fault_code)},
        {"severity", severity_name(f.severity)},
        {"subsystem", std::to_string(f.subsystem_id)},
    };
    out.rows.push_back(std::move(row));
  }
  return QueryError::None;
}

QueryError QueryEngine::run_transitions(const ParsedQuery& q,
                                        QueryResult& out) const {
  if (anomalies_ == nullptr) return QueryError::None;
  const size_t limit = limit_of(q);

  for (const TransitionEvent& t : anomalies_->transitions()) {
    if (out.rows.size() >= limit) break;
    auto leaf = [&](const PredNode& n) -> bool {
      double lhs = 0.0;
      if (n.attr == "from") {
        lhs = static_cast<double>(t.from_state);
      } else if (n.attr == "to") {
        lhs = static_cast<double>(t.to_state);
      } else if (n.attr == "cause") {
        lhs = static_cast<double>(t.cause);
      } else if (n.attr == "device") {
        lhs = static_cast<double>(t.device_id);
      } else if (n.attr == "seq") {
        lhs = static_cast<double>(t.session_seq);
      } else if (n.attr == "ts") {
        lhs = static_cast<double>(t.monotonic_ts);
      } else {
        return false;
      }
      return apply_num(n.op, lhs, n.value.number);
    };
    if (!eval_pred(q.predicate.get(), leaf)) continue;

    QueryResultRow row;
    row.columns = {
        {"device", std::to_string(t.device_id)},
        {"seq", std::to_string(t.session_seq)},
        {"ts", std::to_string(t.monotonic_ts)},
        {"from", std::to_string(t.from_state)},
        {"to", std::to_string(t.to_state)},
        {"cause", std::to_string(t.cause)},
    };
    out.rows.push_back(std::move(row));
  }
  return QueryError::None;
}

QueryError QueryEngine::run_sessions(const ParsedQuery& q,
                                     QueryResult& out) const {
  if (sessions_ == nullptr) return QueryError::None;
  const size_t limit = limit_of(q);

  for (const Session& s : *sessions_) {
    if (out.rows.size() >= limit) break;
    auto leaf = [&](const PredNode& n) -> bool {
      if (n.value.is_string) {
        if (n.attr == "label") return apply_str(n.op, s.label, n.value.text);
        return false;
      }
      double lhs = 0.0;
      if (n.attr == "device") {
        lhs = static_cast<double>(s.device_id);
      } else if (n.attr == "index") {
        lhs = static_cast<double>(s.session_index);
      } else if (n.attr == "closed") {
        lhs = s.closed ? 1.0 : 0.0;
      } else if (n.attr == "duration") {
        lhs = static_cast<double>(s.duration_ns());
      } else {
        return false;
      }
      return apply_num(n.op, lhs, n.value.number);
    };
    if (!eval_pred(q.predicate.get(), leaf)) continue;

    QueryResultRow row;
    row.columns = {
        {"device", std::to_string(s.device_id)},
        {"index", std::to_string(s.session_index)},
        {"closed", std::to_string(s.closed ? 1 : 0)},
        {"duration", std::to_string(s.duration_ns())},
        {"label", s.label},
    };
    out.rows.push_back(std::move(row));
  }
  return QueryError::None;
}

QueryError QueryEngine::run_channels(const ParsedQuery& q,
                                     QueryResult& out) const {
  if (channels_ == nullptr) return QueryError::None;
  const size_t limit = limit_of(q);

  for (uint16_t id : channels_->channels()) {
    if (out.rows.size() >= limit) break;
    const ChannelSeries* series = channels_->series(id);
    if (series == nullptr) continue;
    const size_t versions =
        schemas_ != nullptr ? schemas_->versions(id).size() : 0;

    auto leaf = [&](const PredNode& n) -> bool {
      if (n.value.is_string) {
        if (n.attr == "name")
          return apply_str(n.op, series->schema().name, n.value.text);
        return false;
      }
      double lhs = 0.0;
      if (n.attr == "id") {
        lhs = static_cast<double>(id);
      } else if (n.attr == "samples") {
        lhs = static_cast<double>(series->total_samples());
      } else if (n.attr == "version") {
        lhs = static_cast<double>(series->schema_version());
      } else if (n.attr == "versions") {
        lhs = static_cast<double>(versions);
      } else {
        return false;
      }
      return apply_num(n.op, lhs, n.value.number);
    };
    if (!eval_pred(q.predicate.get(), leaf)) continue;

    QueryResultRow row;
    row.columns = {
        {"id", std::to_string(id)},
        {"name", series->schema().name},
        {"version", std::to_string(series->schema_version())},
        {"samples", std::to_string(series->total_samples())},
        {"versions", std::to_string(versions)},
    };
    out.rows.push_back(std::move(row));
  }
  return QueryError::None;
}

namespace {

QueryError bind_sample_attrs(PredNode* n, const ChannelSchema& schema) {
  if (n == nullptr) return QueryError::None;
  if (n->kind == PredKind::Comparison) {
    if (n->attr == "index") {
      n->sample_col = kSampleIndex;
    } else if (n->attr == "ts") {
      n->sample_col = kSampleTs;
    } else {
      const std::optional<size_t> fi = schema.field_index(n->attr);
      if (!fi) return QueryError::UnknownAttr;
      n->sample_col = static_cast<int>(*fi);
    }
    return QueryError::None;
  }
  for (auto& c : n->children) {
    const QueryError e = bind_sample_attrs(c.get(), schema);
    if (e != QueryError::None) return e;
  }
  return QueryError::None;
}

uint64_t sample_ts(const ChannelSeries& series, size_t index) {
  size_t acc = 0;
  const size_t blocks = series.block_count();
  for (size_t b = 0; b < blocks; ++b) {
    const size_t count = series.block_sample_count(b);
    if (index < acc + count) return series.block_base_ts(b);
    acc += count;
  }
  return 0;
}

}

QueryError QueryEngine::run_samples(const ParsedQuery& q,
                                    QueryResult& out) const {
  if (channels_ == nullptr) return QueryError::None;
  if (!q.has_channel) return QueryError::MissingChannel;
  if (q.channel > 0xFFFFu) return QueryError::NoSuchChannel;

  const ChannelSeries* series =
      channels_->series(static_cast<uint16_t>(q.channel));
  if (series == nullptr) return QueryError::NoSuchChannel;

  const ChannelSchema& schema = series->schema();
  const QueryError berr = bind_sample_attrs(q.predicate.get(), schema);
  if (berr != QueryError::None) return berr;

  const size_t limit = limit_of(q);
  const size_t total = series->total_samples();
  for (size_t index = 0; index < total; ++index) {
    if (out.rows.size() >= limit) break;
    const uint64_t ts = sample_ts(*series, index);
    const std::optional<SampleView> view = series->view(index);

    auto leaf = [&](const PredNode& n) -> bool {
      double lhs = 0.0;
      if (n.sample_col == kSampleIndex) {
        lhs = static_cast<double>(index);
      } else if (n.sample_col == kSampleTs) {
        lhs = static_cast<double>(ts);
      } else {
        if (!view) return false;
        const std::optional<double> v =
            view->get_as_double(static_cast<size_t>(n.sample_col));
        if (!v) return false;
        lhs = *v;
      }
      return apply_num(n.op, lhs, n.value.number);
    };
    if (!eval_pred(q.predicate.get(), leaf)) continue;

    QueryResultRow row;
    row.columns.emplace_back("index", std::to_string(index));
    for (size_t fi = 0; fi < schema.fields.size(); ++fi) {
      std::string val;
      if (view) {
        const std::optional<double> v = view->get_as_double(fi);
        if (v) val = fmt_double(*v);
      }
      row.columns.emplace_back(schema.fields[fi].name, std::move(val));
    }
    row.columns.emplace_back("ts", std::to_string(ts));
    out.rows.push_back(std::move(row));
  }
  return QueryError::None;
}

}
