//
// Created by dear on 2025/12/30.
//

#include "JsonDecode.h"

#include "JsonLex.h"

#include <charconv>
#include <cmath>
#include <new>

namespace fiber::json {
namespace {

using detail::LexStatus;
using detail::Token;
using detail::TokenKind;

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

} // namespace

struct Decoder::Impl {
    static constexpr std::size_t kMaxDepth = 128;

    explicit Impl(const DecodeCallbacks &callbacks) noexcept : callbacks(callbacks) { reset(); }

    void reset() noexcept {
        error = {};
        canceled = false;
        last_bytes_consumed = 0;
        lexer.reset();
        decode_buffer.clear();
        depth = 1;
        state_stack[0] = ParseState::Start;
    }

    [[nodiscard]] DecodeStatus parse(const char *data, std::size_t len, bool final) noexcept {
        last_bytes_consumed = 0;
        if (!data && len > 0) {
            set_state(ParseState::ParseError);
            set_error("input is null", 0);
            return DecodeStatus::Error;
        }
        if (current_state() == ParseState::ParseError) {
            return canceled ? DecodeStatus::Canceled : DecodeStatus::Error;
        }

        const char *input = data ? data : "";
        std::size_t offset = 0;
        while (true) {
            Token tok;
            LexStatus lex_status = lexer.next(input, len, offset, final, tok, error);
            if (lex_status == LexStatus::NeedMore) {
                lexer.finish_chunk(offset);
                last_bytes_consumed = lexer.bytes_consumed();
                if (current_state() == ParseState::ParseComplete) {
                    return DecodeStatus::Complete;
                }
                return DecodeStatus::NeedMore;
            }
            if (lex_status == LexStatus::Error) {
                last_bytes_consumed = offset;
                set_state(ParseState::ParseError);
                return DecodeStatus::Error;
            }
            if (tok.kind == TokenKind::Eof) {
                lexer.finish_chunk(offset);
                last_bytes_consumed = lexer.bytes_consumed();
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
                last_bytes_consumed = offset;
                set_error("trailing garbage after JSON value", tok.offset);
                set_state(ParseState::ParseError);
                return DecodeStatus::Error;
            }
            if (!process_token(tok)) {
                last_bytes_consumed = offset;
                set_state(ParseState::ParseError);
                return canceled ? DecodeStatus::Canceled : DecodeStatus::Error;
            }
        }
    }

    [[nodiscard]] ParseState current_state() const noexcept { return state_stack[depth - 1]; }

    void set_state(ParseState state) noexcept { state_stack[depth - 1] = state; }

    bool set_error(const char *message, std::size_t offset) noexcept {
        if (!error.message) {
            error.message = message;
            error.offset = offset;
        }
        return false;
    }

    bool set_canceled(std::size_t offset) noexcept {
        canceled = true;
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
        if (depth >= kMaxDepth) {
            return set_error("maximum JSON nesting depth exceeded", offset);
        }
        state_stack[depth++] = state;
        return true;
    }

    [[nodiscard]] bool pop_state(std::size_t offset) noexcept {
        if (depth <= 1) {
            return set_error("invalid parser state", offset);
        }
        depth -= 1;
        return true;
    }

    [[nodiscard]] bool dispatch_null(std::size_t offset) noexcept {
        if (callbacks.on_null && callbacks.on_null(callbacks.ctx) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_bool(bool value, std::size_t offset) noexcept {
        if (callbacks.on_bool && callbacks.on_bool(callbacks.ctx, value) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_number(TokenKind kind, const char *data, std::size_t len, std::size_t offset) noexcept {
        if (callbacks.on_number) {
            if (callbacks.on_number(callbacks.ctx, data, len) == 0) {
                return set_canceled(offset);
            }
            return true;
        }

        if (kind == TokenKind::Integer) {
            if (!callbacks.on_integer) {
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
            if (callbacks.on_integer(callbacks.ctx, value) == 0) {
                return set_canceled(offset);
            }
            return true;
        }

        if (!callbacks.on_double) {
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
        if (callbacks.on_double(callbacks.ctx, value) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_string(TokenKind kind, const char *data, std::size_t len, std::size_t offset,
                                       bool object_key) noexcept {
        const char *decoded = data;
        std::size_t decoded_len = len;
        if (kind == TokenKind::StringEscaped &&
            !detail::decode_string(kind, data, len, decode_buffer, decoded, decoded_len, error, offset)) {
            return false;
        }

        if (object_key) {
            if (callbacks.on_object_key && callbacks.on_object_key(callbacks.ctx, decoded, decoded_len) == 0) {
                return set_canceled(offset);
            }
            return true;
        }

        if (callbacks.on_string && callbacks.on_string(callbacks.ctx, decoded, decoded_len) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_start_object(std::size_t offset) noexcept {
        if (callbacks.on_object_start && callbacks.on_object_start(callbacks.ctx) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_end_object(std::size_t offset) noexcept {
        if (callbacks.on_object_end && callbacks.on_object_end(callbacks.ctx) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_start_array(std::size_t offset) noexcept {
        if (callbacks.on_array_start && callbacks.on_array_start(callbacks.ctx) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool dispatch_end_array(std::size_t offset) noexcept {
        if (callbacks.on_array_end && callbacks.on_array_end(callbacks.ctx) == 0) {
            return set_canceled(offset);
        }
        return true;
    }

    [[nodiscard]] bool process_value_token(const Token &tok) noexcept {
        if (!can_accept_value()) {
            return set_error("unexpected token", tok.offset);
        }

        switch (tok.kind) {
            case TokenKind::Null:
                return dispatch_null(tok.offset) && mark_value_started(tok.offset);
            case TokenKind::Bool:
                return dispatch_bool(tok.len > 0 && tok.data[0] == 't', tok.offset) && mark_value_started(tok.offset);
            case TokenKind::Integer:
            case TokenKind::Double:
                return dispatch_number(tok.kind, tok.data, tok.len, tok.offset) && mark_value_started(tok.offset);
            case TokenKind::String:
            case TokenKind::StringEscaped:
                return dispatch_string(tok.kind, tok.data, tok.len, tok.offset, false) &&
                       mark_value_started(tok.offset);
            case TokenKind::ObjectOpen:
                return dispatch_start_object(tok.offset) && mark_value_started(tok.offset) &&
                       push_state(ParseState::ObjectStart, tok.offset);
            case TokenKind::ArrayOpen:
                return dispatch_start_array(tok.offset) && mark_value_started(tok.offset) &&
                       push_state(ParseState::ArrayStart, tok.offset);
            default:
                return set_error("unallowed token at this point in JSON text", tok.offset);
        }
    }

    [[nodiscard]] bool close_container(TokenKind kind, std::size_t offset) noexcept {
        bool ok = false;
        if (kind == TokenKind::ObjectClose) {
            ok = dispatch_end_object(offset);
        } else {
            ok = dispatch_end_array(offset);
        }
        return ok && pop_state(offset);
    }

    [[nodiscard]] bool process_token(const Token &tok) noexcept {
        switch (current_state()) {
            case ParseState::ObjectStart:
            case ParseState::ObjectNeedKey:
                if (tok.kind == TokenKind::ObjectClose && current_state() == ParseState::ObjectStart) {
                    return close_container(tok.kind, tok.offset);
                }
                if (tok.kind != TokenKind::String && tok.kind != TokenKind::StringEscaped) {
                    return set_error("object key must be a string", tok.offset);
                }
                if (!dispatch_string(tok.kind, tok.data, tok.len, tok.offset, true)) {
                    return false;
                }
                set_state(ParseState::ObjectSep);
                return true;
            case ParseState::ObjectSep:
                if (tok.kind != TokenKind::Colon) {
                    return set_error("expected ':' after object key", tok.offset);
                }
                set_state(ParseState::ObjectNeedVal);
                return true;
            case ParseState::ObjectGotVal:
                if (tok.kind == TokenKind::ObjectClose) {
                    return close_container(tok.kind, tok.offset);
                }
                if (tok.kind == TokenKind::Comma) {
                    set_state(ParseState::ObjectNeedKey);
                    return true;
                }
                return set_error("after object value, expected ',' or '}'", tok.offset);
            case ParseState::ArrayStart:
                if (tok.kind == TokenKind::ArrayClose) {
                    return close_container(tok.kind, tok.offset);
                }
                return process_value_token(tok);
            case ParseState::ArrayGotVal:
                if (tok.kind == TokenKind::ArrayClose) {
                    return close_container(tok.kind, tok.offset);
                }
                if (tok.kind == TokenKind::Comma) {
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

    DecodeCallbacks callbacks;
    ParseError error;
    std::size_t last_bytes_consumed = 0;
    detail::Lexer lexer;
    detail::Buffer decode_buffer;
    ParseState state_stack[kMaxDepth] = {};
    std::size_t depth = 1;
    bool canceled = false;
};

Decoder::Decoder(const DecodeCallbacks &callbacks) noexcept : impl_(new(std::nothrow) Impl(callbacks)) {
    if (!impl_) {
        init_error_.message = "out of memory";
        init_error_.offset = 0;
    }
}

Decoder::~Decoder() noexcept { delete impl_; }

void Decoder::reset() noexcept {
    if (impl_) {
        impl_->reset();
    }
}

DecodeStatus Decoder::parse(const char *data, std::size_t len) noexcept {
    if (!impl_) {
        return DecodeStatus::Error;
    }
    return impl_->parse(data, len, false);
}

DecodeStatus Decoder::finish() noexcept {
    if (!impl_) {
        return DecodeStatus::Error;
    }
    return impl_->parse("", 0, true);
}

const ParseError &Decoder::error() const noexcept { return impl_ ? impl_->error : init_error_; }

std::size_t Decoder::bytes_consumed() const noexcept { return impl_ ? impl_->last_bytes_consumed : 0; }

DecodeStatus decode(const char *data, std::size_t len, const DecodeCallbacks &callbacks, ParseError *error) noexcept {
    Decoder decoder(callbacks);
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
