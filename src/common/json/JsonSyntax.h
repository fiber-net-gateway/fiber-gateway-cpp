#ifndef FIBER_JSONSYNTAX_H
#define FIBER_JSONSYNTAX_H

#include <cstddef>
#include <cstdint>

#include "JsonLex.h"
#include "JsonTypes.h"

namespace fiber::json::detail {

enum class SyntaxEventKind : std::uint8_t {
    Ignore,
    Null,
    Bool,
    Integer,
    Double,
    Text,
    ObjectKey,
    StartObject,
    EndObject,
    StartArray,
    EndArray,
};

struct SyntaxEvent {
    SyntaxEventKind kind = SyntaxEventKind::Ignore;
};

template<std::size_t MaxDepth>
class SyntaxMachine {
    static_assert(MaxDepth > 0, "JSON parser depth must be greater than zero");

public:
    SyntaxMachine() noexcept { reset(); }

    void reset() noexcept {
        depth_ = 1;
        state_stack_[0] = State::Start;
        failed_ = false;
    }

    [[nodiscard]] bool complete() const noexcept {
        return !failed_ && depth_ == 1 && state_stack_[0] == State::ParseComplete;
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }

    [[nodiscard]] bool process(const Token &token, SyntaxEvent &event, ParseError &error) noexcept {
        event = {};
        if (failed_) {
            return set_error(error, "invalid parser state", token.offset);
        }

        switch (current_state()) {
            case State::ObjectStart:
            case State::ObjectNeedKey:
                if (token.kind == TokenKind::ObjectClose && current_state() == State::ObjectStart) {
                    event.kind = SyntaxEventKind::EndObject;
                    return pop_state(error, token.offset);
                }
                if (token.kind != TokenKind::String && token.kind != TokenKind::StringEscaped) {
                    return set_error(error, "object key must be a string", token.offset);
                }
                set_state(State::ObjectSep);
                event.kind = SyntaxEventKind::ObjectKey;
                return true;
            case State::ObjectSep:
                if (token.kind != TokenKind::Colon) {
                    return set_error(error, "expected ':' after object key", token.offset);
                }
                set_state(State::ObjectNeedVal);
                return true;
            case State::ObjectGotVal:
                if (token.kind == TokenKind::ObjectClose) {
                    event.kind = SyntaxEventKind::EndObject;
                    return pop_state(error, token.offset);
                }
                if (token.kind == TokenKind::Comma) {
                    set_state(State::ObjectNeedKey);
                    return true;
                }
                return set_error(error, "after object value, expected ',' or '}'", token.offset);
            case State::ArrayStart:
                if (token.kind == TokenKind::ArrayClose) {
                    event.kind = SyntaxEventKind::EndArray;
                    return pop_state(error, token.offset);
                }
                return process_value(token, event, error);
            case State::ArrayGotVal:
                if (token.kind == TokenKind::ArrayClose) {
                    event.kind = SyntaxEventKind::EndArray;
                    return pop_state(error, token.offset);
                }
                if (token.kind == TokenKind::Comma) {
                    set_state(State::ArrayNeedVal);
                    return true;
                }
                return set_error(error, "after array value, expected ',' or ']'", token.offset);
            case State::Start:
            case State::ObjectNeedVal:
            case State::ArrayNeedVal:
                return process_value(token, event, error);
            case State::ParseComplete:
                return set_error(error, "trailing garbage after JSON value", token.offset);
        }
        return set_error(error, "invalid parser state", token.offset);
    }

private:
    enum class State : std::uint8_t {
        Start,
        ParseComplete,
        ObjectStart,
        ObjectNeedKey,
        ObjectSep,
        ObjectNeedVal,
        ObjectGotVal,
        ArrayStart,
        ArrayNeedVal,
        ArrayGotVal,
    };

    [[nodiscard]] State current_state() const noexcept { return state_stack_[depth_ - 1]; }

    void set_state(State state) noexcept { state_stack_[depth_ - 1] = state; }

    [[nodiscard]] bool set_error(ParseError &error, const char *message, std::size_t offset) noexcept {
        failed_ = true;
        if (!error.message) {
            error.message = message;
            error.offset = offset;
        }
        return false;
    }

    [[nodiscard]] bool mark_value_started(ParseError &error, std::size_t offset) noexcept {
        switch (current_state()) {
            case State::Start:
                set_state(State::ParseComplete);
                return true;
            case State::ObjectNeedVal:
                set_state(State::ObjectGotVal);
                return true;
            case State::ArrayNeedVal:
            case State::ArrayStart:
                set_state(State::ArrayGotVal);
                return true;
            default:
                return set_error(error, "unexpected value", offset);
        }
    }

    [[nodiscard]] bool push_state(State state, ParseError &error, std::size_t offset) noexcept {
        if (depth_ >= MaxDepth) {
            return set_error(error, "maximum JSON nesting depth exceeded", offset);
        }
        state_stack_[depth_++] = state;
        return true;
    }

    [[nodiscard]] bool pop_state(ParseError &error, std::size_t offset) noexcept {
        if (depth_ <= 1) {
            return set_error(error, "invalid parser state", offset);
        }
        depth_ -= 1;
        return true;
    }

    [[nodiscard]] bool process_value(const Token &token, SyntaxEvent &event, ParseError &error) noexcept {
        switch (token.kind) {
            case TokenKind::Null:
                event.kind = SyntaxEventKind::Null;
                return mark_value_started(error, token.offset);
            case TokenKind::Bool:
                event.kind = SyntaxEventKind::Bool;
                return mark_value_started(error, token.offset);
            case TokenKind::Integer:
                event.kind = SyntaxEventKind::Integer;
                return mark_value_started(error, token.offset);
            case TokenKind::Double:
                event.kind = SyntaxEventKind::Double;
                return mark_value_started(error, token.offset);
            case TokenKind::String:
            case TokenKind::StringEscaped:
                event.kind = SyntaxEventKind::Text;
                return mark_value_started(error, token.offset);
            case TokenKind::ObjectOpen:
                event.kind = SyntaxEventKind::StartObject;
                return mark_value_started(error, token.offset) && push_state(State::ObjectStart, error, token.offset);
            case TokenKind::ArrayOpen:
                event.kind = SyntaxEventKind::StartArray;
                return mark_value_started(error, token.offset) && push_state(State::ArrayStart, error, token.offset);
            default:
                return set_error(error, "unallowed token at this point in JSON text", token.offset);
        }
    }

    State state_stack_[MaxDepth] = {};
    std::size_t depth_ = 1;
    bool failed_ = false;
};

} // namespace fiber::json::detail

#endif // FIBER_JSONSYNTAX_H
