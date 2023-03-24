#ifndef BLACKSPAN_EXPR_H
#define BLACKSPAN_EXPR_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace blackspan {

enum class ExprError : uint8_t {
  None = 0,
  Empty,
  UnexpectedToken,
  UnexpectedEnd,
  UnbalancedParen,
  UnknownFunction,
  BadArity,
  BadChannelRef,
  TrailingTokens,
  TooComplex,
};

const char* expr_error_str(ExprError e);

enum class FuncId : uint8_t {
  Abs,
  Sqrt,
  Floor,
  Ceil,
  Min,
  Max,
  Clamp,
};

const char* func_name(FuncId f);

enum class ExprKind : uint8_t {
  Number,
  Field,
  ChannelField,
  Unary,
  Binary,
  Call,
};

struct ExprNode {
  ExprKind kind = ExprKind::Number;
  double number = 0.0;
  std::string name;
  uint16_t channel_id = 0;
  char op = 0;
  FuncId func = FuncId::Abs;
  std::vector<std::unique_ptr<ExprNode>> children;
};

struct FieldReference {
  bool cross = false;
  uint16_t channel_id = 0;
  std::string name;
};

using ExprBinding = std::function<std::optional<double>(const FieldReference&)>;

struct ParseResult {
  ExprError error = ExprError::None;
  size_t position = 0;
  std::unique_ptr<ExprNode> ast;

  bool ok() const { return error == ExprError::None; }
};

ParseResult parse_expr(const std::string& text);

bool eval_expr(const ExprNode& node, const ExprBinding& bind, double* out,
               FieldReference* missing);

struct EvalOnceResult {
  ExprError parse_error = ExprError::None;
  size_t parse_position = 0;
  bool bind_failed = false;
  FieldReference missing;
  double value = 0.0;

  bool ok() const { return parse_error == ExprError::None && !bind_failed; }
};

EvalOnceResult evaluate_expr_once(const std::string& expr,
                                  const ExprBinding& bind);

}

#endif
