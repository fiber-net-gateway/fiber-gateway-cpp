#ifndef FIBER_SCRIPT_PARSE_TOKEN_H
#define FIBER_SCRIPT_PARSE_TOKEN_H

#include <cstddef>
#include <string>
#include <string_view>

#include "TokenKind.h"

namespace fiber::script::parse {

struct Token {
    TokenKind kind = TokenKind::Identifier;
    std::size_t start = 0;
    std::size_t end = 0;
    std::string text;

    bool is_identifier() const {
        return kind == TokenKind::Identifier;
    }

    bool is_numeric_relational_operator() const {
        return kind == TokenKind::GT || kind == TokenKind::GE || kind == TokenKind::LT || kind == TokenKind::LE ||
               kind == TokenKind::Eq || kind == TokenKind::Ne || kind == TokenKind::Seq || kind == TokenKind::Sne;
    }

    std::string_view string_value() const {
        return text;
    }
};

} // namespace fiber::script::parse

#endif // FIBER_SCRIPT_PARSE_TOKEN_H
