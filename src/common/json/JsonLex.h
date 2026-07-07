#ifndef FIBER_JSONLEX_H
#define FIBER_JSONLEX_H

#include <cstddef>
#include <cstdint>

#include "JsonDecode.h"

namespace fiber::json::detail {

enum class TokenKind : std::uint8_t {
    Eof,
    Null,
    Bool,
    Integer,
    Double,
    String,
    StringEscaped,
    ObjectOpen,
    ObjectClose,
    ArrayOpen,
    ArrayClose,
    Colon,
    Comma,
};

enum class LexStatus : std::uint8_t {
    Ok,
    NeedMore,
    Error,
};

struct Token {
    TokenKind kind = TokenKind::Eof;
    const char *data = nullptr;
    std::size_t len = 0;
    std::size_t offset = 0;
};

struct Buffer {
    char *data = nullptr;
    std::size_t size = 0;
    std::size_t capacity = 0;

    void clear() noexcept;
    void reset() noexcept;
    [[nodiscard]] bool reserve(std::size_t needed) noexcept;
    [[nodiscard]] bool append(char ch) noexcept;
    [[nodiscard]] bool append(const char *src, std::size_t len) noexcept;
};

class Lexer {
public:
    Lexer() noexcept = default;
    ~Lexer() noexcept;

    Lexer(const Lexer &) = delete;
    Lexer &operator=(const Lexer &) = delete;
    Lexer(Lexer &&) = delete;
    Lexer &operator=(Lexer &&) = delete;

    void reset() noexcept;
    [[nodiscard]] LexStatus next(const char *data, std::size_t len, std::size_t &offset, bool final, Token &out,
                                 ParseError &error) noexcept;
    void finish_chunk(std::size_t consumed) noexcept;
    [[nodiscard]] std::size_t bytes_consumed() const noexcept;
    [[nodiscard]] std::size_t stream_offset() const noexcept;

private:
    enum class ScanStatus : std::uint8_t {
        Complete,
        NeedMore,
        Error,
    };

    enum class Utf8Status : std::uint8_t {
        Ok,
        NeedMore,
        Error,
    };

    struct ScannedToken {
        TokenKind kind = TokenKind::Eof;
        std::size_t token_len = 0;
        std::size_t data_offset = 0;
        std::size_t data_len = 0;
    };

    struct TokenReader {
        const Buffer *prefix = nullptr;
        const char *data = nullptr;
        std::size_t len = 0;
        Buffer *out = nullptr;
        std::size_t prefix_pos = 0;
        std::size_t data_pos = 0;
        bool failed = false;

        [[nodiscard]] bool read(unsigned char &ch) noexcept;
        void unread() noexcept;
        [[nodiscard]] std::size_t input_consumed() const noexcept;
        [[nodiscard]] std::size_t total_consumed() const noexcept;
    };

    [[nodiscard]] LexStatus continue_partial(const char *data, std::size_t len, std::size_t &offset, bool final,
                                             Token &out, ParseError &error) noexcept;
    [[nodiscard]] LexStatus scan_current(const char *data, std::size_t len, std::size_t &offset, bool final,
                                         std::size_t token_offset, Token &out, ParseError &error,
                                         ScanStatus (*scanner)(TokenReader &, bool, ParseError &, std::size_t,
                                                               ScannedToken &) noexcept) noexcept;
    [[nodiscard]] LexStatus scan_current_literal(const char *data, std::size_t len, std::size_t &offset, bool final,
                                                 std::size_t token_offset, const char *literal, TokenKind kind,
                                                 Token &out, ParseError &error) noexcept;
    [[nodiscard]] static ScanStatus scan_string(TokenReader &reader, bool final, ParseError &error,
                                                std::size_t token_offset, ScannedToken &out) noexcept;
    [[nodiscard]] static ScanStatus scan_literal(TokenReader &reader, bool final, ParseError &error,
                                                 std::size_t token_offset, const char *literal, TokenKind kind,
                                                 ScannedToken &out) noexcept;
    [[nodiscard]] static ScanStatus scan_number(TokenReader &reader, bool final, ParseError &error,
                                                std::size_t token_offset, ScannedToken &out) noexcept;
    [[nodiscard]] static Utf8Status scan_utf8_codepoint(TokenReader &reader, unsigned char first, bool final,
                                                        ParseError &error, std::size_t token_offset) noexcept;

    std::size_t stream_offset_ = 0;
    std::size_t last_bytes_consumed_ = 0;
    std::size_t partial_offset_ = 0;
    Buffer partial_;
    Buffer token_storage_;
};

[[nodiscard]] bool decode_string(TokenKind kind, const char *data, std::size_t len, Buffer &scratch, const char *&out,
                                 std::size_t &out_len, ParseError &error, std::size_t offset) noexcept;

} // namespace fiber::json::detail

#endif // FIBER_JSONLEX_H
