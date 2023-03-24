#ifndef BLACKSPAN_QUERY_H
#define BLACKSPAN_QUERY_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "blackspan/anomaly.h"
#include "blackspan/channel.h"
#include "blackspan/schema.h"
#include "blackspan/session.h"

namespace blackspan {

enum class QueryError : uint8_t {
  None = 0,
  Empty,
  UnexpectedToken,
  UnexpectedEnd,
  UnknownEntity,
  UnknownAttr,
  BadOperator,
  BadValue,
  UnterminatedString,
  MissingChannel,
  NoSuchChannel,
  TrailingTokens,
};

const char* query_error_str(QueryError e);

enum class QueryEntity : uint8_t {
  Faults,
  Transitions,
  Sessions,
  Channels,
  Samples,
};

const char* query_entity_str(QueryEntity e);

enum class CompareOp : uint8_t {
  Eq,
  Ne,
  Lt,
  Le,
  Gt,
  Ge,
};

struct QueryValue {
  bool is_string = false;
  double number = 0.0;
  std::string text;
};

enum class PredKind : uint8_t {
  Comparison,
  And,
  Or,
  Not,
};

struct PredNode {
  PredKind kind = PredKind::Comparison;
  std::string attr;
  CompareOp op = CompareOp::Eq;
  QueryValue value;
  int sample_col = -1;
  std::vector<std::unique_ptr<PredNode>> children;
};

struct ParsedQuery {
  QueryEntity entity = QueryEntity::Faults;
  bool has_channel = false;
  uint32_t channel = 0;
  std::unique_ptr<PredNode> predicate;
  bool has_limit = false;
  size_t limit = 0;
};

QueryError parse_query(const std::string& text, ParsedQuery& out,
                       size_t* err_pos);

struct QueryResultRow {
  std::vector<std::pair<std::string, std::string>> columns;
};

struct QueryResult {
  std::string entity;
  std::vector<QueryResultRow> rows;
};

class QueryEngine {
 public:
  QueryEngine(const std::vector<Session>* sessions, const ChannelStore* channels,
              const AnomalyEngine* anomalies, const SchemaRegistry* schemas);

  QueryError run(const std::string& query, QueryResult& out) const;

 private:
  QueryError run_faults(const ParsedQuery& q, QueryResult& out) const;
  QueryError run_transitions(const ParsedQuery& q, QueryResult& out) const;
  QueryError run_sessions(const ParsedQuery& q, QueryResult& out) const;
  QueryError run_channels(const ParsedQuery& q, QueryResult& out) const;
  QueryError run_samples(const ParsedQuery& q, QueryResult& out) const;

  const std::vector<Session>* sessions_;
  const ChannelStore* channels_;
  const AnomalyEngine* anomalies_;
  const SchemaRegistry* schemas_;
};

}

#endif
