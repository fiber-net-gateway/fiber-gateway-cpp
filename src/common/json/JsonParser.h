#ifndef FIBER_JSONPARSER_H
#define FIBER_JSONPARSER_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "JsonLex.h"
#include "JsonSyntax.h"
#include "JsonTypes.h"

namespace fiber::json {

enum class TokenKind : std::uint8_t {
    Null,
    Bool,
    Integer,
    BigNumber,
    Double,
    Text,
    StartObj,
    EndObj,
    StartArr,
    EndArr,
};

enum class TokenRole : std::uint8_t {
    Value,
    ObjectKey,
};

struct Token {
    TokenKind kind = TokenKind::Null;
    TokenRole role = TokenRole::Value;

    union {
        bool bval;
        std::int64_t inum;
        double fnum;
        std::string_view view;
    };

    Token() noexcept : inum(0) {}
};

static_assert(std::is_trivially_copyable_v<std::string_view>);

class JsonParser {
public:
    enum class Status : std::uint8_t {
        Token,
        NeedMore,
        Complete,
        Error,
    };

    JsonParser() noexcept;
    ~JsonParser() noexcept;

    JsonParser(const JsonParser &) = delete;
    JsonParser &operator=(const JsonParser &) = delete;
    JsonParser(JsonParser &&) = delete;
    JsonParser &operator=(JsonParser &&) = delete;

    void reset() noexcept;

    // The input is borrowed until next() returns NeedMore, Complete, or Error.
    // A new chunk may only be fed after the previous chunk has been drained.
    [[nodiscard]] bool feed(const char *data, std::size_t len) noexcept;

    // Marks the current input as the final chunk. Call next() until it returns
    // Complete or Error.
    void finish() noexcept;

    [[nodiscard]] Status next() noexcept;

    // The token and any view it contains remain valid until the next call to
    // next(), feed(), or reset().
    [[nodiscard]] const Token *current_token() const noexcept;

    // Only meaningful while current_token() is non-null. The offset is the
    // token's absolute byte offset in the JSON input stream.
    [[nodiscard]] std::size_t current_offset() const noexcept;

    [[nodiscard]] const ParseError &error() const noexcept;

private:
    static constexpr std::size_t MaxDepth = 128;

    [[nodiscard]] bool set_error(const char *message, std::size_t offset) noexcept;
    void clear_input() noexcept;
    [[nodiscard]] bool materialize(const detail::Token &lex_token, detail::SyntaxEventKind event) noexcept;

    ParseError error_;
    detail::Lexer lexer_;
    detail::SyntaxMachine<MaxDepth> syntax_;
    detail::Buffer decode_buffer_;
    Token current_token_;
    const char *input_ = nullptr;
    std::size_t input_len_ = 0;
    std::size_t input_offset_ = 0;
    std::size_t current_offset_ = 0;
    bool input_active_ = false;
    bool final_ = false;
    bool has_current_token_ = false;
    bool complete_ = false;
    bool failed_ = false;
};

} // namespace fiber::json

#endif // FIBER_JSONPARSER_H
