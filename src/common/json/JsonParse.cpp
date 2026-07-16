#include "JsonParse.h"

#include <charconv>
#include <cmath>
#include <cstring>

namespace fiber::json {
namespace {

[[nodiscard]] const Token *value_token(JsonParser &parser, TokenKind expected, const char *message) noexcept {
    const Token *token = parser.current_token();
    if (!token || token->role != TokenRole::Value || token->kind != expected) {
        (void) parser.fail(message);
        return nullptr;
    }
    return token;
}

} // namespace

ParseStatus parse_null(JsonParser &parser, mem::BufPool & /*pool*/, std::nullptr_t &out) noexcept {
    if (detail::require_finished(parser) != ParseStatus::Done) {
        return ParseStatus::Error;
    }
    if (!value_token(parser, TokenKind::Null, "expected null")) {
        return ParseStatus::Error;
    }
    out = nullptr;
    return ParseStatus::Done;
}

ParseStatus parse_bool(JsonParser &parser, mem::BufPool & /*pool*/, bool &out) noexcept {
    if (detail::require_finished(parser) != ParseStatus::Done) {
        return ParseStatus::Error;
    }
    const Token *token = value_token(parser, TokenKind::Bool, "expected boolean");
    if (!token) {
        return ParseStatus::Error;
    }
    out = token->bval;
    return ParseStatus::Done;
}

ParseStatus parse_text(JsonParser &parser, mem::BufPool &pool, std::string_view &out) noexcept {
    if (detail::require_finished(parser) != ParseStatus::Done) {
        return ParseStatus::Error;
    }
    const Token *token = value_token(parser, TokenKind::Text, "expected string");
    if (!token) {
        return ParseStatus::Error;
    }

    std::string_view result;
    if (!token->view.empty()) {
        auto *data = static_cast<char *>(pool.alloc(token->view.size(), alignof(char)));
        if (!data) {
            return detail::fail(parser, "out of memory");
        }
        std::memcpy(data, token->view.data(), token->view.size());
        result = std::string_view(data, token->view.size());
    }
    out = result;
    return ParseStatus::Done;
}

ParseStatus parse_integer(JsonParser &parser, mem::BufPool & /*pool*/, std::int64_t &out) noexcept {
    if (detail::require_finished(parser) != ParseStatus::Done) {
        return ParseStatus::Error;
    }
    const Token *token = parser.current_token();
    if (!token || token->role != TokenRole::Value) {
        return detail::fail(parser, "expected integer");
    }
    if (token->kind == TokenKind::BigNumber) {
        return detail::fail(parser, "integer out of range");
    }
    if (token->kind != TokenKind::Integer) {
        return detail::fail(parser, "expected integer");
    }
    out = token->inum;
    return ParseStatus::Done;
}

ParseStatus parse_double(JsonParser &parser, mem::BufPool & /*pool*/, double &out) noexcept {
    if (detail::require_finished(parser) != ParseStatus::Done) {
        return ParseStatus::Error;
    }
    const Token *token = parser.current_token();
    if (!token || token->role != TokenRole::Value) {
        return detail::fail(parser, "expected number");
    }

    double result = 0.0;
    switch (token->kind) {
        case TokenKind::Integer:
            result = static_cast<double>(token->inum);
            break;
        case TokenKind::Double:
            result = token->fnum;
            break;
        case TokenKind::BigNumber: {
            auto conversion = std::from_chars(token->view.data(), token->view.data() + token->view.size(), result);
            if (conversion.ec == std::errc::result_out_of_range || !std::isfinite(result)) {
                return detail::fail(parser, "floating point out of range");
            }
            if (conversion.ec != std::errc() || conversion.ptr != token->view.data() + token->view.size()) {
                return detail::fail(parser, "invalid number");
            }
            break;
        }
        default:
            return detail::fail(parser, "expected number");
    }

    out = result;
    return ParseStatus::Done;
}

ParseStatus skip_value(JsonParser &parser, mem::BufPool &pool, std::nullptr_t &out) noexcept {
    out = nullptr;
    return skip_value(parser, pool);
}

ParseStatus skip_value(JsonParser &parser, mem::BufPool & /*pool*/) noexcept {
    if (detail::require_finished(parser) != ParseStatus::Done) {
        return ParseStatus::Error;
    }
    const Token *token = parser.current_token();
    if (!token || token->role != TokenRole::Value) {
        return detail::fail(parser, "expected JSON value");
    }

    switch (token->kind) {
        case TokenKind::Null:
        case TokenKind::Bool:
        case TokenKind::Integer:
        case TokenKind::BigNumber:
        case TokenKind::Double:
        case TokenKind::Text:
            return ParseStatus::Done;
        case TokenKind::StartObj:
        case TokenKind::StartArr:
            break;
        case TokenKind::EndObj:
        case TokenKind::EndArr:
            return detail::fail(parser, "expected JSON value");
    }

    std::size_t depth = 1;
    while (depth > 0) {
        if (detail::next_token(parser, "unexpected end of JSON value") != ParseStatus::Done) {
            return ParseStatus::Error;
        }
        token = parser.current_token();
        if (token->kind == TokenKind::StartObj || token->kind == TokenKind::StartArr) {
            depth += 1;
        } else if (token->kind == TokenKind::EndObj || token->kind == TokenKind::EndArr) {
            depth -= 1;
        }
    }
    return ParseStatus::Done;
}

} // namespace fiber::json
