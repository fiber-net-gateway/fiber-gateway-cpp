#include <fiber/common/json/JsonParser.h>

#include <charconv>
#include <cmath>
#include <memory>
#include <string_view>

namespace fiber::json {

JsonParser::JsonParser() noexcept { reset(); }

JsonParser::~JsonParser() noexcept { decode_buffer_.reset(); }

void JsonParser::reset() noexcept {
    error_ = {};
    lexer_.reset();
    syntax_.reset();
    decode_buffer_.clear();
    current_token_ = {};
    input_ = nullptr;
    input_len_ = 0;
    input_offset_ = 0;
    current_offset_ = 0;
    current_end_offset_ = 0;
    input_active_ = false;
    final_ = false;
    has_current_token_ = false;
    complete_ = false;
    failed_ = false;
}

bool JsonParser::feed(const char *data, std::size_t len) noexcept {
    has_current_token_ = false;
    if (failed_ || complete_) {
        return false;
    }
    if (final_) {
        return set_error("cannot feed after finish", lexer_.stream_offset() + input_offset_);
    }
    if (input_active_) {
        return set_error("previous input chunk is not fully consumed", lexer_.stream_offset() + input_offset_);
    }
    if (!data && len > 0) {
        return set_error("input is null", lexer_.stream_offset());
    }

    input_ = data ? data : "";
    input_len_ = len;
    input_offset_ = 0;
    input_active_ = true;
    return true;
}

void JsonParser::finish() noexcept {
    if (!failed_ && !complete_) {
        final_ = true;
    }
}

JsonParser::Status JsonParser::next() noexcept {
    has_current_token_ = false;
    if (failed_) {
        return Status::Error;
    }
    if (complete_) {
        return Status::Complete;
    }
    if (!input_active_) {
        if (!final_) {
            return Status::NeedMore;
        }
        input_ = "";
        input_len_ = 0;
        input_offset_ = 0;
        input_active_ = true;
    }

    while (true) {
        detail::Token lex_token;
        detail::LexStatus lex_status = lexer_.next(input_, input_len_, input_offset_, final_, lex_token, error_);
        if (lex_status == detail::LexStatus::NeedMore) {
            lexer_.finish_chunk(input_offset_);
            clear_input();
            return Status::NeedMore;
        }
        if (lex_status == detail::LexStatus::Error) {
            failed_ = true;
            clear_input();
            return Status::Error;
        }
        if (lex_token.kind == detail::TokenKind::Eof) {
            lexer_.finish_chunk(input_offset_);
            clear_input();
            if (syntax_.complete()) {
                complete_ = true;
                return Status::Complete;
            }
            (void) set_error("premature EOF", lex_token.offset);
            return Status::Error;
        }

        detail::SyntaxEvent event;
        if (!syntax_.process(lex_token, event, error_)) {
            failed_ = true;
            return Status::Error;
        }
        if (event.kind == detail::SyntaxEventKind::Ignore) {
            continue;
        }
        if (!materialize(lex_token, event.kind)) {
            failed_ = true;
            return Status::Error;
        }

        current_offset_ = lex_token.offset;
        current_end_offset_ = lexer_.stream_offset() + input_offset_;
        has_current_token_ = true;
        return Status::Token;
    }
}

const Token *JsonParser::current_token() const noexcept { return has_current_token_ ? &current_token_ : nullptr; }

std::size_t JsonParser::current_offset() const noexcept { return current_offset_; }

std::size_t JsonParser::current_end_offset() const noexcept { return current_end_offset_; }

const ParseError &JsonParser::error() const noexcept { return error_; }

bool JsonParser::fail(const char *message) noexcept {
    const std::size_t offset = has_current_token_ ? current_offset_ : lexer_.stream_offset() + input_offset_;
    return set_error(message, offset);
}

bool JsonParser::fail(const char *message, std::size_t offset) noexcept { return set_error(message, offset); }

bool JsonParser::set_error(const char *message, std::size_t offset) noexcept {
    failed_ = true;
    if (!error_.message) {
        error_.message = message;
        error_.offset = offset;
    }
    return false;
}

void JsonParser::clear_input() noexcept {
    input_ = nullptr;
    input_len_ = 0;
    input_offset_ = 0;
    input_active_ = false;
}

bool JsonParser::materialize(const detail::Token &lex_token, detail::SyntaxEventKind event) noexcept {
    current_token_.role = event == detail::SyntaxEventKind::ObjectKey ? TokenRole::ObjectKey : TokenRole::Value;

    switch (event) {
        case detail::SyntaxEventKind::Null:
            current_token_.kind = TokenKind::Null;
            current_token_.inum = 0;
            return true;
        case detail::SyntaxEventKind::Bool:
            current_token_.kind = TokenKind::Bool;
            current_token_.bval = lex_token.len > 0 && lex_token.data[0] == 't';
            return true;
        case detail::SyntaxEventKind::Integer: {
            std::int64_t value = 0;
            auto result = std::from_chars(lex_token.data, lex_token.data + lex_token.len, value);
            if (result.ec == std::errc::result_out_of_range) {
                current_token_.kind = TokenKind::BigNumber;
                std::construct_at(&current_token_.view, lex_token.data, lex_token.len);
                return true;
            }
            if (result.ec != std::errc() || result.ptr != lex_token.data + lex_token.len) {
                return set_error("invalid number", lex_token.offset);
            }
            current_token_.kind = TokenKind::Integer;
            current_token_.inum = value;
            return true;
        }
        case detail::SyntaxEventKind::Double: {
            double value = 0.0;
            auto result = std::from_chars(lex_token.data, lex_token.data + lex_token.len, value);
            if (result.ec == std::errc::result_out_of_range || !std::isfinite(value)) {
                current_token_.kind = TokenKind::BigNumber;
                std::construct_at(&current_token_.view, lex_token.data, lex_token.len);
                return true;
            }
            if (result.ec != std::errc() || result.ptr != lex_token.data + lex_token.len) {
                return set_error("invalid number", lex_token.offset);
            }
            current_token_.kind = TokenKind::Double;
            current_token_.fnum = value;
            return true;
        }
        case detail::SyntaxEventKind::Text:
        case detail::SyntaxEventKind::ObjectKey: {
            const char *data = lex_token.data;
            std::size_t len = lex_token.len;
            if (lex_token.kind == detail::TokenKind::StringEscaped &&
                !detail::decode_string(lex_token.kind, lex_token.data, lex_token.len, decode_buffer_, data, len, error_,
                                       lex_token.offset)) {
                return false;
            }
            current_token_.kind = TokenKind::Text;
            std::construct_at(&current_token_.view, data, len);
            return true;
        }
        case detail::SyntaxEventKind::StartObject:
            current_token_.kind = TokenKind::StartObj;
            current_token_.inum = 0;
            return true;
        case detail::SyntaxEventKind::EndObject:
            current_token_.kind = TokenKind::EndObj;
            current_token_.inum = 0;
            return true;
        case detail::SyntaxEventKind::StartArray:
            current_token_.kind = TokenKind::StartArr;
            current_token_.inum = 0;
            return true;
        case detail::SyntaxEventKind::EndArray:
            current_token_.kind = TokenKind::EndArr;
            current_token_.inum = 0;
            return true;
        case detail::SyntaxEventKind::Ignore:
            return set_error("invalid parser state", lex_token.offset);
    }
    return set_error("invalid parser state", lex_token.offset);
}

} // namespace fiber::json
