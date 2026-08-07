//
// Created by dear on 2025/12/30.
//

#ifndef FIBER_JSONDECODE_H
#define FIBER_JSONDECODE_H

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "JsonLex.h"
#include "JsonSyntax.h"
#include "JsonTypes.h"

namespace fiber::json {

enum class DecodeStatus {
    Ok,
    NeedMore,
    Complete,
    Error,
    Canceled,
};

struct DecodeCallbacks {
    void *ctx = nullptr;

    int (*on_null)(void *ctx) noexcept = nullptr;
    int (*on_bool)(void *ctx, bool value) noexcept = nullptr;

    int (*on_number)(void *ctx, const char *data, std::size_t len) noexcept = nullptr;
    int (*on_integer)(void *ctx, std::int64_t value) noexcept = nullptr;
    int (*on_double)(void *ctx, double value) noexcept = nullptr;

    int (*on_string)(void *ctx, const char *data, std::size_t len) noexcept = nullptr;
    int (*on_object_key)(void *ctx, const char *data, std::size_t len) noexcept = nullptr;

    int (*on_object_start)(void *ctx) noexcept = nullptr;
    int (*on_object_end)(void *ctx) noexcept = nullptr;
    int (*on_array_start)(void *ctx) noexcept = nullptr;
    int (*on_array_end)(void *ctx) noexcept = nullptr;
};

template<std::size_t MaxDepth = 128>
class Decoder {
    static_assert(MaxDepth > 0, "JSON decoder depth must be greater than zero");

public:
    explicit Decoder(const DecodeCallbacks &callbacks) noexcept : callbacks_(callbacks) { reset(); }

    ~Decoder() noexcept { decode_buffer_.reset(); }

    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;
    Decoder(Decoder &&) = delete;
    Decoder &operator=(Decoder &&) = delete;

    void reset() noexcept {
        error_ = {};
        canceled_ = false;
        failed_ = false;
        last_bytes_consumed_ = 0;
        lexer_.reset();
        syntax_.reset();
        decode_buffer_.clear();
    }

    [[nodiscard]] DecodeStatus parse(const char *data, std::size_t len) noexcept {
        return parse_internal(data, len, false);
    }

    [[nodiscard]] DecodeStatus finish() noexcept { return parse_internal("", 0, true); }

    [[nodiscard]] const ParseError &error() const noexcept { return error_; }

    [[nodiscard]] std::size_t bytes_consumed() const noexcept { return last_bytes_consumed_; }

private:
    [[nodiscard]] DecodeStatus parse_internal(const char *data, std::size_t len, bool final) noexcept {
        last_bytes_consumed_ = 0;
        if (!data && len > 0) {
            failed_ = true;
            set_error("input is null", 0);
            return DecodeStatus::Error;
        }
        if (failed_) {
            return canceled_ ? DecodeStatus::Canceled : DecodeStatus::Error;
        }

        const char *input = data ? data : "";
        std::size_t offset = 0;
        while (true) {
            detail::Token token;
            detail::LexStatus lex_status = lexer_.next(input, len, offset, final, token, error_);
            if (lex_status == detail::LexStatus::NeedMore) {
                lexer_.finish_chunk(offset);
                last_bytes_consumed_ = lexer_.bytes_consumed();
                if (syntax_.complete()) {
                    return DecodeStatus::Complete;
                }
                return DecodeStatus::NeedMore;
            }
            if (lex_status == detail::LexStatus::Error) {
                last_bytes_consumed_ = offset;
                failed_ = true;
                return DecodeStatus::Error;
            }
            if (token.kind == detail::TokenKind::Eof) {
                lexer_.finish_chunk(offset);
                last_bytes_consumed_ = lexer_.bytes_consumed();
                if (syntax_.complete()) {
                    return DecodeStatus::Complete;
                }
                if (final) {
                    set_error("premature EOF", token.offset);
                    failed_ = true;
                    return DecodeStatus::Error;
                }
                return DecodeStatus::NeedMore;
            }

            detail::SyntaxEvent event;
            if (!syntax_.process(token, event, error_)) {
                last_bytes_consumed_ = offset;
                failed_ = true;
                return DecodeStatus::Error;
            }
            if (event.kind == detail::SyntaxEventKind::Ignore) {
                continue;
            }
            if (!dispatch(event.kind, token)) {
                last_bytes_consumed_ = offset;
                failed_ = true;
                return canceled_ ? DecodeStatus::Canceled : DecodeStatus::Error;
            }
        }
    }

    bool set_error(const char *message, std::size_t offset) noexcept {
        if (!error_.message) {
            error_.message = message;
            error_.offset = offset;
        }
        return false;
    }

    bool set_canceled(std::size_t offset) noexcept {
        canceled_ = true;
        return set_error("parse canceled by callback", offset);
    }

    [[nodiscard]] bool dispatch(detail::SyntaxEventKind event, const detail::Token &token) noexcept {
        switch (event) {
            case detail::SyntaxEventKind::Null:
                if (callbacks_.on_null && callbacks_.on_null(callbacks_.ctx) == 0) {
                    return set_canceled(token.offset);
                }
                return true;
            case detail::SyntaxEventKind::Bool:
                if (callbacks_.on_bool &&
                    callbacks_.on_bool(callbacks_.ctx, token.len > 0 && token.data[0] == 't') == 0) {
                    return set_canceled(token.offset);
                }
                return true;
            case detail::SyntaxEventKind::Integer:
            case detail::SyntaxEventKind::Double:
                return dispatch_number(token);
            case detail::SyntaxEventKind::Text:
                return dispatch_string(token, false);
            case detail::SyntaxEventKind::ObjectKey:
                return dispatch_string(token, true);
            case detail::SyntaxEventKind::StartObject:
                if (callbacks_.on_object_start && callbacks_.on_object_start(callbacks_.ctx) == 0) {
                    return set_canceled(token.offset);
                }
                return true;
            case detail::SyntaxEventKind::EndObject:
                if (callbacks_.on_object_end && callbacks_.on_object_end(callbacks_.ctx) == 0) {
                    return set_canceled(token.offset);
                }
                return true;
            case detail::SyntaxEventKind::StartArray:
                if (callbacks_.on_array_start && callbacks_.on_array_start(callbacks_.ctx) == 0) {
                    return set_canceled(token.offset);
                }
                return true;
            case detail::SyntaxEventKind::EndArray:
                if (callbacks_.on_array_end && callbacks_.on_array_end(callbacks_.ctx) == 0) {
                    return set_canceled(token.offset);
                }
                return true;
            case detail::SyntaxEventKind::Ignore:
                return true;
        }
        return set_error("invalid parser state", token.offset);
    }

    [[nodiscard]] bool dispatch_number(const detail::Token &token) noexcept {
        if (callbacks_.on_number) {
            if (callbacks_.on_number(callbacks_.ctx, token.data, token.len) == 0) {
                return set_canceled(token.offset);
            }
            return true;
        }

        if (token.kind == detail::TokenKind::Integer) {
            if (!callbacks_.on_integer) {
                return true;
            }
            std::int64_t value = 0;
            auto result = std::from_chars(token.data, token.data + token.len, value);
            if (result.ec == std::errc::result_out_of_range) {
                return set_error("integer overflow", token.offset);
            }
            if (result.ec != std::errc() || result.ptr != token.data + token.len) {
                return set_error("invalid number", token.offset);
            }
            if (callbacks_.on_integer(callbacks_.ctx, value) == 0) {
                return set_canceled(token.offset);
            }
            return true;
        }

        if (!callbacks_.on_double) {
            return true;
        }
        double value = 0.0;
        auto result = std::from_chars(token.data, token.data + token.len, value);
        if (result.ec == std::errc::result_out_of_range || !std::isfinite(value)) {
            return set_error("floating point overflow", token.offset);
        }
        if (result.ec != std::errc() || result.ptr != token.data + token.len) {
            return set_error("invalid number", token.offset);
        }
        if (callbacks_.on_double(callbacks_.ctx, value) == 0) {
            return set_canceled(token.offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_string(const detail::Token &token, bool object_key) noexcept {
        const char *decoded = token.data;
        std::size_t decoded_len = token.len;
        if (token.kind == detail::TokenKind::StringEscaped &&
            !detail::decode_string(token.kind, token.data, token.len, decode_buffer_, decoded, decoded_len, error_,
                                   token.offset)) {
            return false;
        }

        if (object_key) {
            if (callbacks_.on_object_key && callbacks_.on_object_key(callbacks_.ctx, decoded, decoded_len) == 0) {
                return set_canceled(token.offset);
            }
            return true;
        }

        if (callbacks_.on_string && callbacks_.on_string(callbacks_.ctx, decoded, decoded_len) == 0) {
            return set_canceled(token.offset);
        }
        return true;
    }

    DecodeCallbacks callbacks_;
    ParseError error_;
    std::size_t last_bytes_consumed_ = 0;
    detail::Lexer lexer_;
    detail::SyntaxMachine<MaxDepth> syntax_;
    detail::Buffer decode_buffer_;
    bool canceled_ = false;
    bool failed_ = false;
};

Decoder(const DecodeCallbacks &) -> Decoder<>;

template<std::size_t MaxDepth = 128>
[[nodiscard]] DecodeStatus decode(const char *data, std::size_t len, const DecodeCallbacks &callbacks,
                                  ParseError *error = nullptr) noexcept {
    Decoder<MaxDepth> decoder(callbacks);
    DecodeStatus status = decoder.parse(data, len);
    if (status == DecodeStatus::NeedMore) {
        status = decoder.finish();
    }
    if (error) {
        *error = decoder.error();
    }
    return status;
}

} // namespace fiber::json

#endif // FIBER_JSONDECODE_H
