#include <cmath>
#include <cstdlib>
#include <utility>

#include "blackspan/query.h"

// Tokenizer and recursive-descent parser for the triage query language. The
// grammar and attribute tables are documented in query.h. The parser latches
// the first error it meets, records the source position, and unwinds without
// throwing; every construct that can be malformed maps to a specific
// QueryError. Attribute validity is checked against the entity here for every
// entity except 'samples', whose field names are resolved later against the
// channel schema (see query_engine.cpp).

namespace blackspan {

const char* query_error_str(QueryError e) {
  switch (e) {
    case QueryError::None:
      return "ok";
    case QueryError::Empty:
      return "empty-query";
    case QueryError::UnexpectedToken:
      return "unexpected-token";
    case QueryError::UnexpectedEnd:
      return "unexpected-end";
    case QueryError::UnknownEntity:
      return "unknown-entity";
    case QueryError::UnknownAttr:
      return "unknown-attribute";
    case QueryError::BadOperator:
      return "bad-operator";
    case QueryError::BadValue:
      return "bad-value";
    case QueryError::UnterminatedString:
      return "unterminated-string";
    case QueryError::MissingChannel:
      return "missing-channel";
    case QueryError::NoSuchChannel:
      return "no-such-channel";
    case QueryError::TrailingTokens:
      return "trailing-tokens";
  }
  return "unknown";
}

const char* query_entity_str(QueryEntity e) {
  switch (e) {
    case QueryEntity::Faults:
      return "faults";
    case QueryEntity::Transitions:
      return "transitions";
    case QueryEntity::Sessions:
      return "sessions";
    case QueryEntity::Channels:
      return "channels";
    case QueryEntity::Samples:
      return "samples";
  }
  return "unknown";
}

namespace {

// The deepest predicate nesting the parser will descend before refusing the
// query. A guard well above any realistic triage predicate that keeps hostile,
// deeply parenthesized input from overflowing the parse stack.
constexpr int kMaxDepth = 256;

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

enum class Tok : uint8_t {
  End,
  Ident,
  Number,
  String,
  Eq,
  Ne,
  Lt,
  Le,
  Gt,
  Ge,
  LParen,
  RParen,
  Invalid,  // an unrecognized character or malformed operator/number run
};

struct QTok {
  Tok kind = Tok::End;
  size_t pos = 0;         // byte offset of the token's first character
  double num = 0.0;       // Number
  std::string text;       // Ident / String content
  bool terminated = true;  // String: a closing quote was seen
};

// Splits the source into tokens, always ending with an End token whose position
// is the input length. Unrecognized characters, malformed numbers, and lone
// operator characters become Invalid tokens so the parser can point at them; an
// unclosed string literal becomes a String token with terminated == false.
std::vector<QTok> tokenize(const std::string& s) {
  std::vector<QTok> out;
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    const char c = s[i];
    if (is_space(c)) {
      ++i;
      continue;
    }
    QTok t;
    t.pos = i;
    if (is_digit(c)) {
      const size_t start = i;
      if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        size_t j = i + 2;
        while (j < n && is_hex(s[j])) ++j;
        if (j > i + 2) {
          const std::string hx = s.substr(i + 2, j - (i + 2));
          t.num = static_cast<double>(std::strtoull(hx.c_str(), nullptr, 16));
          t.kind = Tok::Number;
        } else {
          t.kind = Tok::Invalid;  // "0x" with no hex digits
        }
        i = j;
        out.push_back(std::move(t));
        continue;
      }
      while (i < n && is_digit(s[i])) ++i;
      if (i + 1 < n && s[i] == '.' && is_digit(s[i + 1])) {
        ++i;
        while (i < n && is_digit(s[i])) ++i;
      }
      if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t j = i + 1;
        if (j < n && (s[j] == '+' || s[j] == '-')) ++j;
        if (j < n && is_digit(s[j])) {
          i = j + 1;
          while (i < n && is_digit(s[i])) ++i;
        }
      }
      const std::string num = s.substr(start, i - start);
      const char* cs = num.c_str();
      char* end = nullptr;
      t.num = std::strtod(cs, &end);
      t.kind = (end == cs + num.size()) ? Tok::Number : Tok::Invalid;
      out.push_back(std::move(t));
      continue;
    }
    if (is_alpha(c)) {
      const size_t start = i;
      while (i < n && is_alnum(s[i])) ++i;
      t.kind = Tok::Ident;
      t.text = s.substr(start, i - start);
      out.push_back(std::move(t));
      continue;
    }
    if (c == '\'') {
      const size_t body = i + 1;
      size_t j = body;
      while (j < n && s[j] != '\'') ++j;
      t.kind = Tok::String;
      t.text = s.substr(body, j - body);
      t.terminated = (j < n);
      i = (j < n) ? j + 1 : j;  // consume the closing quote when present
      out.push_back(std::move(t));
      continue;
    }
    switch (c) {
      case '=':
        if (i + 1 < n && s[i + 1] == '=') {
          t.kind = Tok::Eq;
          ++i;
        } else {
          t.kind = Tok::Invalid;
        }
        break;
      case '!':
        if (i + 1 < n && s[i + 1] == '=') {
          t.kind = Tok::Ne;
          ++i;
        } else {
          t.kind = Tok::Invalid;
        }
        break;
      case '<':
        if (i + 1 < n && s[i + 1] == '=') {
          t.kind = Tok::Le;
          ++i;
        } else {
          t.kind = Tok::Lt;
        }
        break;
      case '>':
        if (i + 1 < n && s[i + 1] == '=') {
          t.kind = Tok::Ge;
          ++i;
        } else {
          t.kind = Tok::Gt;
        }
        break;
      case '(':
        t.kind = Tok::LParen;
        break;
      case ')':
        t.kind = Tok::RParen;
        break;
      default:
        t.kind = Tok::Invalid;
        break;
    }
    ++i;
    out.push_back(std::move(t));
  }
  QTok end_tok;
  end_tok.kind = Tok::End;
  end_tok.pos = n;
  out.push_back(std::move(end_tok));
  return out;
}

// The kind of value an attribute accepts, used to type-check the right-hand
// side of a comparison at parse time.
enum class AttrType : uint8_t {
  Unknown,   // the attribute is not defined for the entity
  Numeric,   // compares against an integer / float / hex literal
  String,    // compares against a quoted string literal
  Severity,  // compares against a severity word or an integer level
};

AttrType classify_attr(QueryEntity e, const std::string& a) {
  switch (e) {
    case QueryEntity::Faults:
      if (a == "severity") return AttrType::Severity;
      if (a == "code" || a == "subsystem" || a == "device" || a == "seq" ||
          a == "ts")
        return AttrType::Numeric;
      return AttrType::Unknown;
    case QueryEntity::Transitions:
      if (a == "from" || a == "to" || a == "cause" || a == "device" ||
          a == "seq" || a == "ts")
        return AttrType::Numeric;
      return AttrType::Unknown;
    case QueryEntity::Sessions:
      if (a == "device" || a == "index" || a == "closed" || a == "duration")
        return AttrType::Numeric;
      if (a == "label") return AttrType::String;
      return AttrType::Unknown;
    case QueryEntity::Channels:
      if (a == "id" || a == "samples" || a == "version" || a == "versions")
        return AttrType::Numeric;
      if (a == "name") return AttrType::String;
      return AttrType::Unknown;
    case QueryEntity::Samples:
      // Field names are resolved against the channel schema at evaluation time;
      // every attribute on a sample row is numeric.
      return AttrType::Numeric;
  }
  return AttrType::Unknown;
}

bool severity_level(const std::string& w, double* out) {
  if (w == "info") {
    *out = 0.0;
    return true;
  }
  if (w == "warning") {
    *out = 1.0;
    return true;
  }
  if (w == "error") {
    *out = 2.0;
    return true;
  }
  if (w == "critical") {
    *out = 3.0;
    return true;
  }
  return false;
}

std::unique_ptr<PredNode> new_pred(PredKind k) {
  auto n = std::make_unique<PredNode>();
  n->kind = k;
  return n;
}

// A recursive-descent parser over a tokenized query. It latches the first error
// it meets and unwinds; every recursive step checks `error` before building a
// node. `entity` is fixed before predicate parsing so attribute validity can be
// checked as each comparison is read.
struct Parser {
  explicit Parser(const std::vector<QTok>& t) : toks(t) {}

  const std::vector<QTok>& toks;
  size_t idx = 0;
  int depth = 0;
  QueryEntity entity = QueryEntity::Faults;
  QueryError error = QueryError::None;
  size_t error_pos = 0;

  const QTok& peek() const { return toks[idx]; }
  void advance() {
    if (idx + 1 < toks.size()) ++idx;
  }
  void fail(QueryError e, size_t pos) {
    if (error == QueryError::None) {
      error = e;
      error_pos = pos;
    }
  }

  // The error a token warrants where a specific construct was expected: a
  // premature End, an unterminated string, or otherwise an unexpected token.
  QueryError token_error(const QTok& t) const {
    if (t.kind == Tok::End) return QueryError::UnexpectedEnd;
    if (t.kind == Tok::String && !t.terminated)
      return QueryError::UnterminatedString;
    return QueryError::UnexpectedToken;
  }

  bool accept_keyword(const char* kw) {
    if (peek().kind == Tok::Ident && peek().text == kw) {
      advance();
      return true;
    }
    return false;
  }

  // Reads a non-negative integer literal no greater than `max`. A missing or
  // non-integer token fails with BadValue (or UnexpectedEnd at end of input).
  bool take_uint(uint64_t max, uint64_t* out) {
    const QTok& t = peek();
    if (t.kind == Tok::End) {
      fail(QueryError::UnexpectedEnd, t.pos);
      return false;
    }
    if (t.kind != Tok::Number || !(t.num >= 0.0) ||
        std::floor(t.num) != t.num || t.num > static_cast<double>(max)) {
      fail(QueryError::BadValue, t.pos);
      return false;
    }
    *out = static_cast<uint64_t>(t.num);
    advance();
    return true;
  }

  bool parse_op(CompareOp* out) {
    switch (peek().kind) {
      case Tok::Eq:
        *out = CompareOp::Eq;
        break;
      case Tok::Ne:
        *out = CompareOp::Ne;
        break;
      case Tok::Lt:
        *out = CompareOp::Lt;
        break;
      case Tok::Le:
        *out = CompareOp::Le;
        break;
      case Tok::Gt:
        *out = CompareOp::Gt;
        break;
      case Tok::Ge:
        *out = CompareOp::Ge;
        break;
      case Tok::End:
        fail(QueryError::UnexpectedEnd, peek().pos);
        return false;
      default:
        fail(QueryError::BadOperator, peek().pos);
        return false;
    }
    advance();
    return true;
  }

  // Reads the right-hand literal and type-checks it against the attribute kind.
  bool parse_value(AttrType at, QueryValue* out) {
    const QTok& t = peek();
    switch (t.kind) {
      case Tok::End:
        fail(QueryError::UnexpectedEnd, t.pos);
        return false;
      case Tok::String: {
        if (!t.terminated) {
          fail(QueryError::UnterminatedString, t.pos);
          return false;
        }
        if (at == AttrType::String) {
          out->is_string = true;
          out->text = t.text;
          advance();
          return true;
        }
        if (at == AttrType::Severity) {  // a quoted severity word is accepted
          double lvl = 0.0;
          if (!severity_level(t.text, &lvl)) {
            fail(QueryError::BadValue, t.pos);
            return false;
          }
          out->number = lvl;
          advance();
          return true;
        }
        fail(QueryError::BadValue, t.pos);  // numeric attr, string literal
        return false;
      }
      case Tok::Ident: {  // a bare word is a value only for a severity attribute
        if (at != AttrType::Severity) {
          fail(QueryError::BadValue, t.pos);
          return false;
        }
        double lvl = 0.0;
        if (!severity_level(t.text, &lvl)) {
          fail(QueryError::BadValue, t.pos);
          return false;
        }
        out->number = lvl;
        advance();
        return true;
      }
      case Tok::Number: {
        if (at == AttrType::String) {
          fail(QueryError::BadValue, t.pos);
          return false;
        }
        out->number = t.num;
        advance();
        return true;
      }
      default:
        fail(QueryError::BadValue, t.pos);
        return false;
    }
  }

  std::unique_ptr<PredNode> parse_or();
  std::unique_ptr<PredNode> parse_and();
  std::unique_ptr<PredNode> parse_not();
  std::unique_ptr<PredNode> parse_comparison();
};

// Bounds predicate recursion: constructed on entry to each recursive rule, it
// reports whether descending one more level is within the limit and releases
// the level when the rule returns.
struct DepthGuard {
  Parser& p;
  bool ok;
  explicit DepthGuard(Parser& parser) : p(parser) {
    ok = (++p.depth <= kMaxDepth);
  }
  ~DepthGuard() { --p.depth; }
};

std::unique_ptr<PredNode> Parser::parse_or() {
  auto left = parse_and();
  if (error != QueryError::None) return nullptr;
  while (accept_keyword("or")) {
    auto right = parse_and();
    if (error != QueryError::None) return nullptr;
    auto node = new_pred(PredKind::Or);
    node->children.push_back(std::move(left));
    node->children.push_back(std::move(right));
    left = std::move(node);
  }
  return left;
}

std::unique_ptr<PredNode> Parser::parse_and() {
  auto left = parse_not();
  if (error != QueryError::None) return nullptr;
  while (accept_keyword("and")) {
    auto right = parse_not();
    if (error != QueryError::None) return nullptr;
    auto node = new_pred(PredKind::And);
    node->children.push_back(std::move(left));
    node->children.push_back(std::move(right));
    left = std::move(node);
  }
  return left;
}

std::unique_ptr<PredNode> Parser::parse_not() {
  DepthGuard g(*this);
  if (!g.ok) {
    fail(QueryError::UnexpectedToken, peek().pos);
    return nullptr;
  }
  if (accept_keyword("not")) {
    auto child = parse_not();
    if (error != QueryError::None) return nullptr;
    auto node = new_pred(PredKind::Not);
    node->children.push_back(std::move(child));
    return node;
  }
  if (peek().kind == Tok::LParen) {
    advance();
    auto inner = parse_or();
    if (error != QueryError::None) return nullptr;
    if (peek().kind != Tok::RParen) {
      fail(token_error(peek()), peek().pos);
      return nullptr;
    }
    advance();
    return inner;
  }
  return parse_comparison();
}

std::unique_ptr<PredNode> Parser::parse_comparison() {
  const QTok& a = peek();
  if (a.kind != Tok::Ident) {
    fail(token_error(a), a.pos);
    return nullptr;
  }
  const AttrType at = classify_attr(entity, a.text);
  if (at == AttrType::Unknown) {
    fail(QueryError::UnknownAttr, a.pos);
    return nullptr;
  }
  auto node = new_pred(PredKind::Comparison);
  node->attr = a.text;
  advance();
  if (!parse_op(&node->op)) return nullptr;
  if (!parse_value(at, &node->value)) return nullptr;
  return node;
}

}  // namespace

QueryError parse_query(const std::string& text, ParsedQuery& out,
                       size_t* err_pos) {
  out.entity = QueryEntity::Faults;
  out.has_channel = false;
  out.channel = 0;
  out.predicate.reset();
  out.has_limit = false;
  out.limit = 0;

  const std::vector<QTok> toks = tokenize(text);
  if (toks.size() == 1 && toks[0].kind == Tok::End) {
    if (err_pos) *err_pos = 0;
    return QueryError::Empty;
  }

  Parser p(toks);

  if (!p.accept_keyword("select")) {
    const QueryError e = p.token_error(p.peek());
    if (err_pos) *err_pos = p.peek().pos;
    return e;
  }

  // entity
  if (p.peek().kind != Tok::Ident) {
    const QueryError e = p.token_error(p.peek());
    if (err_pos) *err_pos = p.peek().pos;
    return e;
  }
  const std::string ent = p.peek().text;
  QueryEntity entity;
  if (ent == "faults") {
    entity = QueryEntity::Faults;
  } else if (ent == "transitions") {
    entity = QueryEntity::Transitions;
  } else if (ent == "sessions") {
    entity = QueryEntity::Sessions;
  } else if (ent == "channels") {
    entity = QueryEntity::Channels;
  } else if (ent == "samples") {
    entity = QueryEntity::Samples;
  } else {
    if (err_pos) *err_pos = p.peek().pos;
    return QueryError::UnknownEntity;
  }
  out.entity = entity;
  p.entity = entity;
  p.advance();
  const size_t after_entity_pos = p.peek().pos;

  // optional 'from channel N'
  if (p.accept_keyword("from")) {
    if (!p.accept_keyword("channel")) {
      const QueryError e = p.token_error(p.peek());
      if (err_pos) *err_pos = p.peek().pos;
      return e;
    }
    uint64_t ch = 0;
    if (!p.take_uint(0xFFFFFFFFull, &ch)) {
      if (err_pos) *err_pos = p.error_pos;
      return p.error;
    }
    out.has_channel = true;
    out.channel = static_cast<uint32_t>(ch);
  }

  // optional 'where predicate'
  if (p.accept_keyword("where")) {
    out.predicate = p.parse_or();
    if (p.error != QueryError::None) {
      if (err_pos) *err_pos = p.error_pos;
      return p.error;
    }
  }

  // optional 'limit N'
  if (p.accept_keyword("limit")) {
    uint64_t lim = 0;
    if (!p.take_uint((1ull << 53), &lim)) {
      if (err_pos) *err_pos = p.error_pos;
      return p.error;
    }
    out.has_limit = true;
    out.limit = static_cast<size_t>(lim);
  }

  // Nothing may follow a complete query.
  if (p.peek().kind != Tok::End) {
    const QTok& t = p.peek();
    QueryError e = QueryError::TrailingTokens;
    if (t.kind == Tok::String && !t.terminated)
      e = QueryError::UnterminatedString;
    if (err_pos) *err_pos = t.pos;
    return e;
  }

  // A 'samples' query must name the channel it reads from.
  if (entity == QueryEntity::Samples && !out.has_channel) {
    if (err_pos) *err_pos = after_entity_pos;
    return QueryError::MissingChannel;
  }

  return QueryError::None;
}

}  // namespace blackspan
