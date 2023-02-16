#include "blackspan/expr.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

namespace blackspan {

const char* expr_error_str(ExprError e) {
  switch (e) {
    case ExprError::None:
      return "ok";
    case ExprError::Empty:
      return "empty-expression";
    case ExprError::UnexpectedToken:
      return "unexpected-token";
    case ExprError::UnexpectedEnd:
      return "unexpected-end";
    case ExprError::UnbalancedParen:
      return "unbalanced-paren";
    case ExprError::UnknownFunction:
      return "unknown-function";
    case ExprError::BadArity:
      return "bad-arity";
    case ExprError::BadChannelRef:
      return "bad-channel-ref";
    case ExprError::TrailingTokens:
      return "trailing-tokens";
    case ExprError::TooComplex:
      return "too-complex";
  }
  return "unknown";
}

const char* func_name(FuncId f) {
  switch (f) {
    case FuncId::Abs:
      return "abs";
    case FuncId::Sqrt:
      return "sqrt";
    case FuncId::Floor:
      return "floor";
    case FuncId::Ceil:
      return "ceil";
    case FuncId::Min:
      return "min";
    case FuncId::Max:
      return "max";
    case FuncId::Clamp:
      return "clamp";
  }
  return "unknown";
}

namespace {

// The deepest nesting the parser will descend before refusing an expression.
// A guard well above any realistic formula that keeps hostile, deeply nested
// input from overflowing the parse or evaluation stack.
constexpr int kMaxDepth = 128;

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }

enum class TokKind : uint8_t {
  End,
  Number,
  Ident,
  Plus,
  Minus,
  Star,
  Slash,
  LParen,
  RParen,
  Comma,
  Dot,
  Invalid,  // an unrecognized or malformed character run
};

struct Token {
  TokKind kind = TokKind::End;
  size_t pos = 0;      // byte offset of the token's first character
  double number = 0.0;  // Number
  std::string text;     // Ident
};

// Splits the source into tokens, always ending with an End token whose position
// is the length of the input. Unrecognized characters and malformed numeric
// runs become Invalid tokens so the parser can point at them.
std::vector<Token> tokenize(const std::string& s) {
  std::vector<Token> out;
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    const char c = s[i];
    if (is_space(c)) {
      ++i;
      continue;
    }
    Token t;
    t.pos = i;
    if (is_digit(c)) {
      const size_t start = i;
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
      const double v = std::strtod(cs, &end);
      t.number = v;
      t.kind = (end == cs + num.size()) ? TokKind::Number : TokKind::Invalid;
      out.push_back(std::move(t));
      continue;
    }
    if (is_alpha(c)) {
      const size_t start = i;
      while (i < n && is_alnum(s[i])) ++i;
      t.kind = TokKind::Ident;
      t.text = s.substr(start, i - start);
      out.push_back(std::move(t));
      continue;
    }
    switch (c) {
      case '+':
        t.kind = TokKind::Plus;
        break;
      case '-':
        t.kind = TokKind::Minus;
        break;
      case '*':
        t.kind = TokKind::Star;
        break;
      case '/':
        t.kind = TokKind::Slash;
        break;
      case '(':
        t.kind = TokKind::LParen;
        break;
      case ')':
        t.kind = TokKind::RParen;
        break;
      case ',':
        t.kind = TokKind::Comma;
        break;
      case '.':
        t.kind = TokKind::Dot;
        break;
      default:
        t.kind = TokKind::Invalid;
        break;
    }
    ++i;
    out.push_back(std::move(t));
  }
  Token end_tok;
  end_tok.kind = TokKind::End;
  end_tok.pos = n;
  out.push_back(std::move(end_tok));
  return out;
}

std::optional<FuncId> lookup_func(const std::string& name) {
  if (name == "abs") return FuncId::Abs;
  if (name == "sqrt") return FuncId::Sqrt;
  if (name == "floor") return FuncId::Floor;
  if (name == "ceil") return FuncId::Ceil;
  if (name == "min") return FuncId::Min;
  if (name == "max") return FuncId::Max;
  if (name == "clamp") return FuncId::Clamp;
  return std::nullopt;
}

size_t func_arity(FuncId f) {
  switch (f) {
    case FuncId::Abs:
    case FuncId::Sqrt:
    case FuncId::Floor:
    case FuncId::Ceil:
      return 1;
    case FuncId::Min:
    case FuncId::Max:
      return 2;
    case FuncId::Clamp:
      return 3;
  }
  return 0;
}

// Parses a ch<digits> channel-reference prefix, e.g. "ch5" -> 5. Rejects a
// missing "ch", no digits, non-digit trailing characters, or a value that
// would not fit a channel id.
bool parse_channel_prefix(const std::string& s, uint16_t* out) {
  if (s.size() < 3) return false;
  if (s[0] != 'c' || s[1] != 'h') return false;
  uint32_t v = 0;
  for (size_t i = 2; i < s.size(); ++i) {
    if (!is_digit(s[i])) return false;
    v = v * 10 + static_cast<uint32_t>(s[i] - '0');
    if (v > 0xFFFFu) return false;
  }
  *out = static_cast<uint16_t>(v);
  return true;
}

std::unique_ptr<ExprNode> new_node(ExprKind k) {
  auto n = std::make_unique<ExprNode>();
  n->kind = k;
  return n;
}

// A recursive-descent parser over a tokenized formula. It latches the first
// error it meets and unwinds, so every recursive step checks `error` before
// building a node.
struct Parser {
  explicit Parser(const std::vector<Token>& t) : toks(t) {}

  const std::vector<Token>& toks;
  size_t idx = 0;
  int depth = 0;
  ExprError error = ExprError::None;
  size_t error_pos = 0;

  const Token& peek() const { return toks[idx]; }
  void advance() {
    if (idx + 1 < toks.size()) ++idx;
  }
  void fail(ExprError e, size_t pos) {
    if (error == ExprError::None) {
      error = e;
      error_pos = pos;
    }
  }

  std::unique_ptr<ExprNode> parse_additive();
  std::unique_ptr<ExprNode> parse_multiplicative();
  std::unique_ptr<ExprNode> parse_unary();
  std::unique_ptr<ExprNode> parse_primary();
};

// Bounds recursion depth: constructed on entry to each recursive rule, it
// reports whether descending one more level is still within the limit and
// releases the level when the rule returns.
struct DepthGuard {
  Parser& p;
  bool ok;
  explicit DepthGuard(Parser& parser) : p(parser) {
    ok = (++p.depth <= kMaxDepth);
  }
  ~DepthGuard() { --p.depth; }
};

std::unique_ptr<ExprNode> Parser::parse_additive() {
  DepthGuard g(*this);
  if (!g.ok) {
    fail(ExprError::TooComplex, peek().pos);
    return nullptr;
  }
  auto left = parse_multiplicative();
  if (error != ExprError::None) return nullptr;
  while (peek().kind == TokKind::Plus || peek().kind == TokKind::Minus) {
    const char op = peek().kind == TokKind::Plus ? '+' : '-';
    advance();
    auto right = parse_multiplicative();
    if (error != ExprError::None) return nullptr;
    auto node = new_node(ExprKind::Binary);
    node->op = op;
    node->children.push_back(std::move(left));
    node->children.push_back(std::move(right));
    left = std::move(node);
  }
  return left;
}

std::unique_ptr<ExprNode> Parser::parse_multiplicative() {
  auto left = parse_unary();
  if (error != ExprError::None) return nullptr;
  while (peek().kind == TokKind::Star || peek().kind == TokKind::Slash) {
    const char op = peek().kind == TokKind::Star ? '*' : '/';
    advance();
    auto right = parse_unary();
    if (error != ExprError::None) return nullptr;
    auto node = new_node(ExprKind::Binary);
    node->op = op;
    node->children.push_back(std::move(left));
    node->children.push_back(std::move(right));
    left = std::move(node);
  }
  return left;
}

std::unique_ptr<ExprNode> Parser::parse_unary() {
  DepthGuard g(*this);
  if (!g.ok) {
    fail(ExprError::TooComplex, peek().pos);
    return nullptr;
  }
  if (peek().kind == TokKind::Minus || peek().kind == TokKind::Plus) {
    const char op = peek().kind == TokKind::Minus ? '-' : '+';
    advance();
    auto child = parse_unary();
    if (error != ExprError::None) return nullptr;
    auto node = new_node(ExprKind::Unary);
    node->op = op;
    node->children.push_back(std::move(child));
    return node;
  }
  return parse_primary();
}

std::unique_ptr<ExprNode> Parser::parse_primary() {
  const Token& t = peek();
  switch (t.kind) {
    case TokKind::Number: {
      auto node = new_node(ExprKind::Number);
      node->number = t.number;
      advance();
      return node;
    }
    case TokKind::LParen: {
      const size_t open_pos = t.pos;
      advance();
      auto inner = parse_additive();
      if (error != ExprError::None) return nullptr;
      if (peek().kind != TokKind::RParen) {
        fail(ExprError::UnbalancedParen,
             peek().kind == TokKind::End ? open_pos : peek().pos);
        return nullptr;
      }
      advance();
      return inner;
    }
    case TokKind::Ident: {
      const std::string name = t.text;
      const size_t name_pos = t.pos;
      advance();
      if (peek().kind == TokKind::LParen) {
        const std::optional<FuncId> fid = lookup_func(name);
        if (!fid) {
          fail(ExprError::UnknownFunction, name_pos);
          return nullptr;
        }
        advance();  // consume '('
        std::vector<std::unique_ptr<ExprNode>> args;
        if (peek().kind != TokKind::RParen) {
          while (true) {
            auto arg = parse_additive();
            if (error != ExprError::None) return nullptr;
            args.push_back(std::move(arg));
            if (peek().kind == TokKind::Comma) {
              advance();
              continue;
            }
            break;
          }
        }
        if (peek().kind != TokKind::RParen) {
          fail(ExprError::UnbalancedParen,
               peek().kind == TokKind::End ? name_pos : peek().pos);
          return nullptr;
        }
        advance();  // consume ')'
        if (args.size() != func_arity(*fid)) {
          fail(ExprError::BadArity, name_pos);
          return nullptr;
        }
        auto node = new_node(ExprKind::Call);
        node->func = *fid;
        node->children = std::move(args);
        return node;
      }
      if (peek().kind == TokKind::Dot) {
        uint16_t chid = 0;
        if (!parse_channel_prefix(name, &chid)) {
          fail(ExprError::BadChannelRef, name_pos);
          return nullptr;
        }
        advance();  // consume '.'
        if (peek().kind != TokKind::Ident) {
          fail(peek().kind == TokKind::End ? ExprError::UnexpectedEnd
                                           : ExprError::UnexpectedToken,
               peek().pos);
          return nullptr;
        }
        auto node = new_node(ExprKind::ChannelField);
        node->channel_id = chid;
        node->name = peek().text;
        advance();
        return node;
      }
      auto node = new_node(ExprKind::Field);
      node->name = name;
      return node;
    }
    case TokKind::End:
      fail(ExprError::UnexpectedEnd, t.pos);
      return nullptr;
    default:
      // Star, Slash, Comma, RParen, Dot, or Invalid where a value is required.
      fail(ExprError::UnexpectedToken, t.pos);
      return nullptr;
  }
}

}  // namespace

ParseResult parse_expr(const std::string& text) {
  ParseResult res;
  const std::vector<Token> toks = tokenize(text);
  if (toks.size() == 1 && toks[0].kind == TokKind::End) {
    res.error = ExprError::Empty;
    res.position = 0;
    return res;
  }
  Parser p(toks);
  auto node = p.parse_additive();
  if (p.error != ExprError::None) {
    res.error = p.error;
    res.position = p.error_pos;
    return res;
  }
  if (p.peek().kind != TokKind::End) {
    if (p.peek().kind == TokKind::RParen) {
      res.error = ExprError::UnbalancedParen;
    } else if (p.peek().kind == TokKind::Invalid) {
      // An unrecognized character is a lexical error wherever it lands.
      res.error = ExprError::UnexpectedToken;
    } else {
      res.error = ExprError::TrailingTokens;
    }
    res.position = p.peek().pos;
    return res;
  }
  res.ast = std::move(node);
  return res;
}

bool eval_expr(const ExprNode& node, const ExprBinding& bind, double* out,
               FieldReference* missing) {
  switch (node.kind) {
    case ExprKind::Number:
      *out = node.number;
      return true;
    case ExprKind::Field: {
      FieldReference ref;
      ref.cross = false;
      ref.name = node.name;
      const std::optional<double> v = bind(ref);
      if (!v) {
        if (missing) *missing = ref;
        return false;
      }
      *out = *v;
      return true;
    }
    case ExprKind::ChannelField: {
      FieldReference ref;
      ref.cross = true;
      ref.channel_id = node.channel_id;
      ref.name = node.name;
      const std::optional<double> v = bind(ref);
      if (!v) {
        if (missing) *missing = ref;
        return false;
      }
      *out = *v;
      return true;
    }
    case ExprKind::Unary: {
      double c = 0.0;
      if (!eval_expr(*node.children[0], bind, &c, missing)) return false;
      *out = (node.op == '-') ? -c : c;
      return true;
    }
    case ExprKind::Binary: {
      double a = 0.0;
      double b = 0.0;
      if (!eval_expr(*node.children[0], bind, &a, missing)) return false;
      if (!eval_expr(*node.children[1], bind, &b, missing)) return false;
      switch (node.op) {
        case '+':
          *out = a + b;
          break;
        case '-':
          *out = a - b;
          break;
        case '*':
          *out = a * b;
          break;
        case '/':
          // A zero divisor is a defined, non-fatal result: NaN.
          *out = (b == 0.0) ? std::numeric_limits<double>::quiet_NaN() : a / b;
          break;
        default:
          *out = std::numeric_limits<double>::quiet_NaN();
          break;
      }
      return true;
    }
    case ExprKind::Call: {
      double a[3] = {0.0, 0.0, 0.0};
      const size_t na = node.children.size();
      for (size_t i = 0; i < na && i < 3; ++i) {
        if (!eval_expr(*node.children[i], bind, &a[i], missing)) return false;
      }
      switch (node.func) {
        case FuncId::Abs:
          *out = std::fabs(a[0]);
          break;
        case FuncId::Sqrt:
          *out = std::sqrt(a[0]);
          break;
        case FuncId::Floor:
          *out = std::floor(a[0]);
          break;
        case FuncId::Ceil:
          *out = std::ceil(a[0]);
          break;
        case FuncId::Min:
          *out = (a[1] < a[0]) ? a[1] : a[0];
          break;
        case FuncId::Max:
          *out = (a[1] > a[0]) ? a[1] : a[0];
          break;
        case FuncId::Clamp:
          *out = (a[0] < a[1]) ? a[1] : (a[0] > a[2] ? a[2] : a[0]);
          break;
      }
      return true;
    }
  }
  *out = 0.0;
  return true;
}

EvalOnceResult evaluate_expr_once(const std::string& expr,
                                  const ExprBinding& bind) {
  EvalOnceResult res;
  ParseResult pr = parse_expr(expr);
  if (!pr.ok()) {
    res.parse_error = pr.error;
    res.parse_position = pr.position;
    return res;
  }
  double value = 0.0;
  FieldReference missing;
  if (!eval_expr(*pr.ast, bind, &value, &missing)) {
    res.bind_failed = true;
    res.missing = missing;
    return res;
  }
  res.value = value;
  return res;
}

}  // namespace blackspan
