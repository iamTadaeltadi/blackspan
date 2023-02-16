#ifndef BLACKSPAN_EXPR_H
#define BLACKSPAN_EXPR_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The expr subsystem is the small arithmetic language that derived channels are
// written in. It tokenizes an ASCII formula, parses it with recursive descent
// into an AST that honors standard operator precedence, and evaluates that AST
// against a caller-supplied binding that resolves field references to values.
//
// Grammar (EBNF), whitespace-insensitive:
//   expr    := term (('+' | '-') term)*
//   term    := factor (('*' | '/') factor)*
//   factor  := ('+' | '-') factor | primary
//   primary := number | '(' expr ')' | call | field
//   call    := ident '(' [ expr (',' expr)* ] ')'
//   field   := ident                     // a field on the primary channel
//            | 'ch' digits '.' ident      // a field on another channel by id
//   number  := digits ['.' digits] [('e'|'E') ['+'|'-'] digits]
//
// Functions: abs / sqrt / floor / ceil (1 arg), min / max (2 args), clamp
// (3 args). Nothing here throws: a malformed formula yields an ExprError with
// the byte position at which parsing failed, deeply nested input is rejected
// rather than overflowing the stack, and a reference that cannot be resolved at
// evaluation time is reported through the binding, never as a crash.

namespace blackspan {

// Reasons a formula failed to parse. Every value is reachable from some input.
enum class ExprError : uint8_t {
  None = 0,
  Empty,            // the expression had no tokens
  UnexpectedToken,  // a token appeared where a value was expected
  UnexpectedEnd,    // the expression ended in the middle of a subexpression
  UnbalancedParen,  // a '(' had no matching ')', or a stray ')' appeared
  UnknownFunction,  // a call named a function this evaluator does not define
  BadArity,         // a known function received the wrong number of arguments
  BadChannelRef,    // an identifier before '.' was not of the form ch<digits>
  TrailingTokens,   // tokens remained after an otherwise complete expression
  TooComplex,       // the expression nested deeper than the parser will accept
};

// Human-readable form of an ExprError. Never null.
const char* expr_error_str(ExprError e);

// The built-in functions the language recognizes.
enum class FuncId : uint8_t {
  Abs,
  Sqrt,
  Floor,
  Ceil,
  Min,
  Max,
  Clamp,
};

// Lowercase spelling of a function ("abs", "clamp", ...). Never null.
const char* func_name(FuncId f);

// The kinds of node an expression AST is built from.
enum class ExprKind : uint8_t {
  Number,        // a numeric literal (number)
  Field,         // a field on the primary channel (name)
  ChannelField,  // a field on another channel (channel_id, name)
  Unary,         // a unary operator (op in {'+','-'}, one child)
  Binary,        // a binary operator (op in {'+','-','*','/'}, two children)
  Call,          // a function call (func, children are the arguments)
};

// One node of a parsed expression. Which members are meaningful is selected by
// `kind`; unrelated members keep their defaults. Children are owned so the tree
// frees itself.
struct ExprNode {
  ExprKind kind = ExprKind::Number;
  double number = 0.0;         // Number
  std::string name;            // Field / ChannelField field name
  uint16_t channel_id = 0;     // ChannelField source channel
  char op = 0;                 // Unary / Binary operator character
  FuncId func = FuncId::Abs;   // Call
  std::vector<std::unique_ptr<ExprNode>> children;  // operands / call arguments
};

// A reference to a channel field appearing in an expression. When `cross` is
// false the reference is a bare identifier naming a field on the primary
// channel; when true it is a ch<id>.<field> reference to another channel, and
// channel_id names that channel.
struct FieldReference {
  bool cross = false;
  uint16_t channel_id = 0;
  std::string name;
};

// Resolves a field reference to a value, returning nullopt when the reference
// cannot be bound (an unknown field, an absent channel, a missing sample). A
// binding is consulted once per field occurrence per evaluation.
using ExprBinding = std::function<std::optional<double>(const FieldReference&)>;

// The outcome of parsing a formula. On success `ast` owns the parsed tree and
// `error` is None; on failure `ast` is null and `position` is the byte offset
// in the source text at which parsing gave up.
struct ParseResult {
  ExprError error = ExprError::None;
  size_t position = 0;
  std::unique_ptr<ExprNode> ast;

  bool ok() const { return error == ExprError::None; }
};

// Parses an infix formula into an AST. Robust to every malformed input: the
// result carries a specific ExprError and a source position rather than
// throwing or crashing.
ParseResult parse_expr(const std::string& text);

// Evaluates an AST, writing the result to *out. Returns false and, if `missing`
// is non-null, records the reference that failed to bind when the binding
// returns nullopt for some field; the tree is otherwise fully evaluated.
// Division by a zero divisor yields a NaN result and is not a failure.
bool eval_expr(const ExprNode& node, const ExprBinding& bind, double* out,
               FieldReference* missing);

// The outcome of parsing and evaluating a formula in one shot. `ok()` is true
// only when the formula both parsed and had every reference bound; otherwise
// either parse_error (with parse_position) or bind_failed (with `missing`)
// explains the failure. `value` holds the result on success.
struct EvalOnceResult {
  ExprError parse_error = ExprError::None;
  size_t parse_position = 0;
  bool bind_failed = false;
  FieldReference missing;
  double value = 0.0;

  bool ok() const { return parse_error == ExprError::None && !bind_failed; }
};

// Parses `expr` and, if it parsed, evaluates it once against `bind`. A
// convenience for callers and tests that want a single value from a formula
// without holding onto an AST.
EvalOnceResult evaluate_expr_once(const std::string& expr,
                                  const ExprBinding& bind);

}  // namespace blackspan

#endif  // BLACKSPAN_EXPR_H
