#ifndef FIBER_SCRIPT_PARSE_TOKEN_KIND_H
#define FIBER_SCRIPT_PARSE_TOKEN_KIND_H

#include <cstdint>

namespace fiber::script::parse {

enum class TokenKind : std::uint8_t {
    LiteralInt,
    LiteralLong,
    LiteralHexInt,
    LiteralHexLong,
    LiteralString,
    LiteralReal,
    LiteralRealFloat,
    LParen,
    RParen,
    Comma,
    Identifier,
    Colon,
    Hash,
    RSquare,
    LSquare,
    LCurly,
    RCurly,
    Expand,
    Dot,
    Plus,
    Star,
    Minus,
    QMark,
    Project,
    Div,
    GE,
    GT,
    LE,
    LT,
    Seq,
    Sne,
    Eq,
    Ne,
    Mod,
    Semicolon,
    Not,
    Assign,
    Tilde,
    Select,
    SymbolicOr,
    SymbolicAnd,
    Inc,
    Dec,
};

const char *token_kind_name(TokenKind kind);

} // namespace fiber::script::parse

#endif // FIBER_SCRIPT_PARSE_TOKEN_KIND_H
