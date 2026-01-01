#include "Operator.h"

#include <optional>

namespace fiber::script::ast {

std::string_view operator_payload(Operator op) {
    switch (op) {
        case Operator::Add:
            return "+";
        case Operator::Minus:
            return "-";
        case Operator::Multiply:
            return "*";
        case Operator::Divide:
            return "/";
        case Operator::Modulo:
            return "%";
        case Operator::Match:
            return "~";
        case Operator::And:
            return "&&";
        case Operator::Or:
            return "||";
        case Operator::Lt:
            return "<";
        case Operator::Lte:
            return "<=";
        case Operator::Gt:
            return ">";
        case Operator::Gte:
            return ">=";
        case Operator::Eq:
            return "==";
        case Operator::Seq:
            return "===";
        case Operator::Ne:
            return "!=";
        case Operator::Sne:
            return "!==";
        case Operator::Not:
            return "!";
        case Operator::Typeof:
            return "typeof";
        case Operator::In:
            return "in";
    }
    return "";
}

std::optional<Operator> operator_from_token(parse::TokenKind kind) {
    switch (kind) {
        case parse::TokenKind::Plus:
            return Operator::Add;
        case parse::TokenKind::Minus:
            return Operator::Minus;
        case parse::TokenKind::Star:
            return Operator::Multiply;
        case parse::TokenKind::Div:
            return Operator::Divide;
        case parse::TokenKind::Mod:
            return Operator::Modulo;
        case parse::TokenKind::Tilde:
            return Operator::Match;
        case parse::TokenKind::SymbolicAnd:
            return Operator::And;
        case parse::TokenKind::SymbolicOr:
            return Operator::Or;
        case parse::TokenKind::LT:
            return Operator::Lt;
        case parse::TokenKind::LE:
            return Operator::Lte;
        case parse::TokenKind::GT:
            return Operator::Gt;
        case parse::TokenKind::GE:
            return Operator::Gte;
        case parse::TokenKind::Eq:
            return Operator::Eq;
        case parse::TokenKind::Seq:
            return Operator::Seq;
        case parse::TokenKind::Ne:
            return Operator::Ne;
        case parse::TokenKind::Sne:
            return Operator::Sne;
        case parse::TokenKind::Not:
            return Operator::Not;
        default:
            return std::nullopt;
    }
}

std::optional<Operator> operator_from_identity(std::string_view name) {
    if (name == "typeof") {
        return Operator::Typeof;
    }
    if (name == "in") {
        return Operator::In;
    }
    return std::nullopt;
}

} // namespace fiber::script::ast
