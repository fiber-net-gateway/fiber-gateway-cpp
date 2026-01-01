#ifndef FIBER_SCRIPT_AST_OPERATOR_H
#define FIBER_SCRIPT_AST_OPERATOR_H

#include <optional>
#include <string_view>

#include "../parse/TokenKind.h"

namespace fiber::script::ast {

enum class Operator : std::uint8_t {
    Add,
    Minus,
    Multiply,
    Divide,
    Modulo,
    Match,
    And,
    Or,
    Lt,
    Lte,
    Gt,
    Gte,
    Eq,
    Seq,
    Ne,
    Sne,
    Not,
    Typeof,
    In,
};

std::string_view operator_payload(Operator op);
std::optional<Operator> operator_from_token(parse::TokenKind kind);
std::optional<Operator> operator_from_identity(std::string_view name);

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_OPERATOR_H
