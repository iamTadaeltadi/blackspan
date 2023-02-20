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

// The query subsystem is a small triage query language over the model the
// decoder reconstructs. It tokenizes a one-line query, parses it with recursive
// descent into a typed request, and evaluates that request against the
// subsystem outputs already produced by earlier stages (the session list, the
// channel store, the anomaly timeline, and the schema registry). It never reads
// the raw byte stream and never mutates the stores it reads.
//
// Grammar (whitespace-insensitive, keywords lowercase):
//   query      := 'select' entity ['from' 'channel' int] ['where' predicate]
//                 ['limit' int]
//   entity     := 'faults' | 'transitions' | 'sessions' | 'channels'
//               | 'samples'
//   predicate  := or_expr
//   or_expr    := and_expr ('or' and_expr)*
//   and_expr   := not_expr ('and' not_expr)*
//   not_expr   := 'not' not_expr | comparison | '(' predicate ')'
//   comparison := attr op value
//   op         := '==' | '!=' | '<' | '<=' | '>' | '>='
//   value      := int | 0xHEX | float | 'string'
//
// Attributes recognized per entity (numeric unless noted):
//   faults      : severity (info/warning/error/critical or int), code,
//                 subsystem, device, seq, ts
//   transitions : from, to, cause, device, seq, ts
//   sessions    : device, index, closed (0/1), duration, label (string)
//   channels    : id, samples, version, versions, name (string)
//   samples     : any field name of the channel's schema, plus index and ts
//                 (a 'samples' query requires a 'from channel N' clause)
//
// Nothing here throws: an empty, malformed, or unsatisfiable query yields a
// specific QueryError (and, at parse time, the byte offset at which parsing
// gave up) rather than a crash. A query whose source store is absent evaluates
// to zero rows, not an error.

namespace blackspan {

// Reasons a query failed to parse or evaluate. Every value is reachable from
// some input. Positions accompany the parse-time reasons; NoSuchChannel is the
// one value produced only at evaluation time (it depends on the loaded model).
enum class QueryError : uint8_t {
  None = 0,
  Empty,              // the query had no tokens
  UnexpectedToken,    // a token appeared where the grammar expected another
  UnexpectedEnd,      // the query ended in the middle of a construct
  UnknownEntity,      // the entity name is not one the language defines
  UnknownAttr,        // the attribute is not defined for the queried entity
  BadOperator,        // a comparison operator was expected but not found
  BadValue,           // a value was malformed or of the wrong kind for the attr
  UnterminatedString, // a quoted string literal had no closing quote
  MissingChannel,     // a 'samples' query lacked its required channel clause
  NoSuchChannel,      // the named channel has no series in the store
  TrailingTokens,     // tokens remained after an otherwise complete query
};

// Human-readable form of a QueryError. Never null.
const char* query_error_str(QueryError e);

// The top-level things a query can select over.
enum class QueryEntity : uint8_t {
  Faults,
  Transitions,
  Sessions,
  Channels,
  Samples,
};

// Lowercase spelling of an entity ("faults", "samples", ...). Never null.
const char* query_entity_str(QueryEntity e);

// The comparison operators a predicate leaf may use.
enum class CompareOp : uint8_t {
  Eq,  // ==
  Ne,  // !=
  Lt,  // <
  Le,  // <=
  Gt,  // >
  Ge,  // >=
};

// A parsed literal value. `is_string` selects which member is meaningful:
// string comparisons read `text`, numeric comparisons read `number`. Severity
// words and integer/float/hex literals are all resolved to `number` at parse
// time so evaluation is uniform.
struct QueryValue {
  bool is_string = false;
  double number = 0.0;
  std::string text;
};

// The kinds of node a predicate tree is built from.
enum class PredKind : uint8_t {
  Comparison,  // attr op value (a leaf)
  And,         // all children must hold
  Or,          // some child must hold
  Not,         // the single child must not hold
};

// One node of a parsed predicate. Which members are meaningful is selected by
// `kind`; Comparison uses attr/op/value, the others use children. Children are
// owned so the tree frees itself.
struct PredNode {
  PredKind kind = PredKind::Comparison;
  std::string attr;                 // Comparison: attribute name
  CompareOp op = CompareOp::Eq;     // Comparison: operator
  QueryValue value;                 // Comparison: right-hand literal
  int sample_col = -1;              // Comparison: resolved 'samples' column
  std::vector<std::unique_ptr<PredNode>> children;  // And / Or / Not operands
};

// A fully parsed query request. `predicate` is null when the query has no
// 'where' clause; `has_channel` / `has_limit` gate the optional integers.
struct ParsedQuery {
  QueryEntity entity = QueryEntity::Faults;
  bool has_channel = false;
  uint32_t channel = 0;
  std::unique_ptr<PredNode> predicate;
  bool has_limit = false;
  size_t limit = 0;
};

// Parses `text` into `out`. On success returns QueryError::None and leaves
// *err_pos untouched. On failure returns a specific QueryError, writes the byte
// offset at which parsing gave up to *err_pos (when non-null), and leaves `out`
// in a partially-built but safe-to-destroy state. Never throws.
QueryError parse_query(const std::string& text, ParsedQuery& out,
                       size_t* err_pos);

// One output row: an ordered list of (column-name, formatted-value) pairs. The
// column set is fixed per entity so rows of one result are uniform.
struct QueryResultRow {
  std::vector<std::pair<std::string, std::string>> columns;
};

// The result of running a query: the entity name it selected over and the rows
// that satisfied its predicate, in model order and capped by any 'limit'.
struct QueryResult {
  std::string entity;
  std::vector<QueryResultRow> rows;
};

// Evaluates queries against a decoded model. The engine borrows the four
// subsystem outputs by pointer and never takes ownership; any of them may be
// null, in which case a query over that entity yields zero rows rather than a
// crash. The borrowed stores must outlive the engine.
class QueryEngine {
 public:
  QueryEngine(const std::vector<Session>* sessions, const ChannelStore* channels,
              const AnomalyEngine* anomalies, const SchemaRegistry* schemas);

  // Parses and evaluates `query`, writing the matching rows to `out`. Returns
  // QueryError::None on success (including a well-formed query that matched no
  // rows), or a specific QueryError for a malformed query or an unresolvable
  // channel or attribute. `out` is cleared before use.
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

}  // namespace blackspan

#endif  // BLACKSPAN_QUERY_H
