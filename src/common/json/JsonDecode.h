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

namespace fiber::json {

struct ParseError {
    const char *message = nullptr;
    std::size_t offset = 0;
};

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
        last_bytes_consumed_ = 0;
        lexer_.reset();
        decode_buffer_.clear();
        depth_ = 1;
        state_stack_[0] = ParseState::Start;
    }

    [[nodiscard]] DecodeStatus parse(const char *data, std::size_t len) noexcept {
        return parse_internal(data, len, false);
    }

    [[nodiscard]] DecodeStatus finish() noexcept { return parse_internal("", 0, true); }

    [[nodiscard]] const ParseError &error() const noexcept { return error_; }

    [[nodiscard]] std::size_t bytes_consumed() const noexcept { return last_bytes_consumed_; }

private:
    enum class ParseState : std::uint8_t {
        Start,
        ParseComplete,
        ParseError,
        ObjectStart,
        ObjectNeedKey,
        ObjectSep,
        ObjectNeedVal,
        ObjectGotVal,
        ArrayStart,
        ArrayNeedVal,
        ArrayGotVal,
    };

    [[nodiscard]] DecodeStatus parse_internal(const char *data, std::size_t len, bool final) noexcept {
        last_bytes_consumed_ = 0;
        if (!data && len > 0) {
            set_state(ParseState::ParseError);
            set_error("input is null", 0);
            return DecodeStatus::Error;
        }
        if (current_state() == ParseState::ParseError) {
            return canceled_ ? DecodeStatus::Canceled : DecodeStatus::Error;
        }

        const char *input = data ? data : "";
        std::size_t offset = 0;
        while (true) {
            detail::Token tok;
            detail::LexStatus lex_status = lexer_.next(input, len, offset, final, tok, error_);
            if (lex_status == detail::LexStatus::NeedMore) {
                lexer_.finish_chunk(offset);
                last_bytes_consumed_ = lexer_.bytes_consumed();
                if (current_state() == ParseState::ParseComplete) {
                    return DecodeStatus::Complete;
                }
                return DecodeStatus::NeedMore;
            }
            if (lex_status == detail::LexStatus::Error) {
                last_bytes_consumed_ = offset;
                set_state(ParseState::ParseError);
                return DecodeStatus::Error;
            }
            if (tok.kind == detail::TokenKind::Eof) {
                lexer_.finish_chunk(offset);
                last_bytes_consumed_ = lexer_.bytes_consumed();
                if (current_state() == ParseState::ParseComplete) {
                    return DecodeStatus::Complete;
                }
                if (final) {
                    set_error("premature EOF", tok.offset);
                    set_state(ParseState::ParseError);
                    return DecodeStatus::Error;
                }
                return DecodeStatus::NeedMore;
            }
            if (current_state() == ParseState::ParseComplete) {
                last_bytes_consumed_ = offset;
                set_error("trailing garbage after JSON value", tok.offset);
                set_state(ParseState::ParseError);
                return DecodeStatus::Error;
            }
            if (!process_token(tok)) {
                last_bytes_consumed_ = offset;
                set_state(ParseState::ParseError);
                return canceled_ ? DecodeStatus::Canceled : DecodeStatus::Error;
            }
        }
    }

    [[nodiscard]] ParseState current_state() const noexcept { return state_stack_[depth_ - 1]; }

    void set_state(ParseState state) noexcept { state_stack_[depth_ - 1] = state; }

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

    [[nodiscard]] bool can_accept_value() const noexcept {
        ParseState state = current_state();
        return state == ParseState::Start || state == ParseState::ObjectNeedVal || state == ParseState::ArrayNeedVal ||
               state == ParseState::ArrayStart;
    }

    [[nodiscard]] bool mark_value_started(std::size_t offset) noexcept {
        switch (current_state()) {
            case ParseState::Start:
                set_state(ParseState::ParseComplete);
                return true;
            case ParseState::ObjectNeedVal:
                set_state(ParseState::ObjectGotVal);
                return true;
            case ParseState::ArrayNeedVal:
            case ParseState::ArrayStart:
                set_state(ParseState::ArrayGotVal);
                return true;
            default:
                return set_error("unexpected value", offset);
        }
    }

    [[nodiscard]] bool push_state(ParseState state, std::size_t offset) noexcept {
        if (depth_ >= MaxDepth) {
            return set_error("maximum JSON nesting depth exceeded", offset);
        }
        state_stack_[depth_++] = state;
        return true;
    }

    [[nodiscard]] bool pop_state(std::size_t offset) noexcept {
        if (depth_ <= 1) {
            return set_error("invalid parser state", offset);
        }
        depth_ -= 1;
        return true;
    }

    [[nodiscard]] bool dispatch_null(std::size_t offset) noexcept {
        if (callbacks_.on_null && callbacks_.on_null(callbacks_.ctx) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_bool(bool value, std::size_t offset) noexcept {
        if (callbacks_.on_bool && callbacks_.on_bool(callbacks_.ctx, value) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_number(detail::TokenKind kind, const char *data, std::size_t len,
                                       std::size_t offset) noexcept {
        if (callbacks_.on_number) {
            if (callbacks_.on_number(callbacks_.ctx, data, len) == 0) {
                return set_canceled(offset);
            }
            return true;
        }

        if (kind == detail::TokenKind::Integer) {
            if (!callbacks_.on_integer) {
                return true;
            }
            std::int64_t value = 0;
            auto result = std::from_chars(data, data + len, value);
            if (result.ec == std::errc::result_out_of_range) {
                return set_error("integer overflow", offset);
            }
            if (result.ec != std::errc() || result.ptr != data + len) {
                return set_error("invalid number", offset);
            }
            if (callbacks_.on_integer(callbacks_.ctx, value) == 0) {
                return set_canceled(offset);
            }
            return true;
        }

        if (!callbacks_.on_double) {
            return true;
        }
        double value = 0.0;
        auto result = std::from_chars(data, data + len, value);
        if (result.ec == std::errc::result_out_of_range || !std::isfinite(value)) {
            return set_error("floating point overflow", offset);
        }
        if (result.ec != std::errc() || result.ptr != data + len) {
            return set_error("invalid number", offset);
        }
        if (callbacks_.on_double(callbacks_.ctx, value) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_string(detail::TokenKind kind, const char *data, std::size_t len, std::size_t offset,
                                       bool object_key) noexcept {
        const char *decoded = data;
        std::size_t decoded_len = len;
        if (kind == detail::TokenKind::StringEscaped &&
            !detail::decode_string(kind, data, len, decode_buffer_, decoded, decoded_len, error_, offset)) {
            return false;
        }

        if (object_key) {
            if (callbacks_.on_object_key && callbacks_.on_object_key(callbacks_.ctx, decoded, decoded_len) == 0) {
                return set_canceled(offset);
            }
            return true;
        }

        if (callbacks_.on_string && callbacks_.on_string(callbacks_.ctx, decoded, decoded_len) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_start_object(std::size_t offset) noexcept {
        if (callbacks_.on_object_start && callbacks_.on_object_start(callbacks_.ctx) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_end_object(std::size_t offset) noexcept {
        if (callbacks_.on_object_end && callbacks_.on_object_end(callbacks_.ctx) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_start_array(std::size_t offset) noexcept {
        if (callbacks_.on_array_start && callbacks_.on_array_start(callbacks_.ctx) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_end_array(std::size_t offset) noexcept {
        if (callbacks_.on_array_end && callbacks_.on_array_end(callbacks_.ctx) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool process_value_token(const detail::Token &tok) noexcept {
        if (!can_accept_value()) {
            return set_error("unexpected token", tok.offset);
        }

        switch (tok.kind) {
            case detail::TokenKind::Null:
                return dispatch_null(tok.offset) && mark_value_started(tok.offset);
            case detail::TokenKind::Bool:
                return dispatch_bool(tok.len > 0 && tok.data[0] == 't', tok.offset) && mark_value_started(tok.offset);
            case detail::TokenKind::Integer:
            case detail::TokenKind::Double:
                return dispatch_number(tok.kind, tok.data, tok.len, tok.offset) && mark_value_started(tok.offset);
            case detail::TokenKind::String:
            case detail::TokenKind::StringEscaped:
                return dispatch_string(tok.kind, tok.data, tok.len, tok.offset, false) &&
                       mark_value_started(tok.offset);
            case detail::TokenKind::ObjectOpen:
                return dispatch_start_object(tok.offset) && mark_value_started(tok.offset) &&
                       push_state(ParseState::ObjectStart, tok.offset);
            case detail::TokenKind::ArrayOpen:
                return dispatch_start_array(tok.offset) && mark_value_started(tok.offset) &&
                       push_state(ParseState::ArrayStart, tok.offset);
            default:
                return set_error("unallowed token at this point in JSON text", tok.offset);
        }
    }

    [[nodiscard]] bool close_container(detail::TokenKind kind, std::size_t offset) noexcept {
        bool ok = false;
        if (kind == detail::TokenKind::ObjectClose) {
            ok = dispatch_end_object(offset);
        } else {
            ok = dispatch_end_array(offset);
        }
        return ok && pop_state(offset);
    }

    [[nodiscard]] bool process_token(const detail::Token &tok) noexcept {
        switch (current_state()) {
            case ParseState::ObjectStart:
            case ParseState::ObjectNeedKey:
                if (tok.kind == detail::TokenKind::ObjectClose && current_state() == ParseState::ObjectStart) {
                    return close_container(tok.kind, tok.offset);
                }
                if (tok.kind != detail::TokenKind::String && tok.kind != detail::TokenKind::StringEscaped) {
                    return set_error("object key must be a string", tok.offset);
                }
                if (!dispatch_string(tok.kind, tok.data, tok.len, tok.offset, true)) {
                    return false;
                }
                set_state(ParseState::ObjectSep);
                return true;
            case ParseState::ObjectSep:
                if (tok.kind != detail::TokenKind::Colon) {
                    return set_error("expected ':' after object key", tok.offset);
                }
                set_state(ParseState::ObjectNeedVal);
                return true;
            case ParseState::ObjectGotVal:
                if (tok.kind == detail::TokenKind::ObjectClose) {
                    return close_container(tok.kind, tok.offset);
                }
                if (tok.kind == detail::TokenKind::Comma) {
                    set_state(ParseState::ObjectNeedKey);
                    return true;
                }
                return set_error("after object value, expected ',' or '}'", tok.offset);
            case ParseState::ArrayStart:
                if (tok.kind == detail::TokenKind::ArrayClose) {
                    return close_container(tok.kind, tok.offset);
                }
                return process_value_token(tok);
            case ParseState::ArrayGotVal:
                if (tok.kind == detail::TokenKind::ArrayClose) {
                    return close_container(tok.kind, tok.offset);
                }
                if (tok.kind == detail::TokenKind::Comma) {
                    set_state(ParseState::ArrayNeedVal);
                    return true;
                }
                return set_error("after array value, expected ',' or ']'", tok.offset);
            case ParseState::Start:
            case ParseState::ObjectNeedVal:
            case ParseState::ArrayNeedVal:
                return process_value_token(tok);
            case ParseState::ParseComplete:
                return set_error("trailing garbage after JSON value", tok.offset);
            case ParseState::ParseError:
                return set_error("invalid parser state", tok.offset);
        }
        return set_error("invalid parser state", tok.offset);
    }

    DecodeCallbacks callbacks_;
    ParseError error_;
    std::size_t last_bytes_consumed_ = 0;
    detail::Lexer lexer_;
    detail::Buffer decode_buffer_;
    ParseState state_stack_[MaxDepth] = {};
    std::size_t depth_ = 1;
    bool canceled_ = false;
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
