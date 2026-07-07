//
// Created by dear on 2025/12/30.
//

#include "JsonDecode.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fiber::json {
namespace {

constexpr std::size_t kInitialContainerCapacity = 4;

enum class TokenKind {
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

enum class LexStatus {
    Ok,
    NeedMore,
    Error,
};

enum class ScanStatus {
    Complete,
    NeedMore,
    Error,
};

enum class ParseState {
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

struct Token {
    TokenKind kind = TokenKind::Eof;
    const char *data = nullptr;
    std::size_t len = 0;
    std::size_t offset = 0;
};

struct ScannedToken {
    TokenKind kind = TokenKind::Eof;
    std::size_t token_len = 0;
    std::size_t data_offset = 0;
    std::size_t data_len = 0;
};

bool set_parse_error(ParseError &error, const char *message, std::size_t offset) {
    if (error.message.empty()) {
        error.message = message;
        error.offset = offset;
    }
    return false;
}

bool is_ws(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; }

bool is_number_delimiter(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == ',' || ch == ']' || ch == '}';
}

bool is_digit(unsigned char ch) { return ch >= '0' && ch <= '9'; }

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

void append_code_unit(DecodedString &out, char16_t unit) {
    if (out.is_byte && unit <= 0xFF) {
        out.bytes.push_back(static_cast<std::uint8_t>(unit));
        return;
    }
    if (out.is_byte) {
        out.is_byte = false;
        out.u16.reserve(out.bytes.size() + 1);
        for (std::uint8_t byte: out.bytes) {
            out.u16.push_back(static_cast<char16_t>(byte));
        }
        out.bytes.clear();
    }
    out.u16.push_back(unit);
}

void append_codepoint(DecodedString &out, std::uint32_t codepoint) {
    if (codepoint <= 0xFFFF) {
        append_code_unit(out, static_cast<char16_t>(codepoint));
        return;
    }
    std::uint32_t value = codepoint - 0x10000;
    append_code_unit(out, static_cast<char16_t>(0xD800 + (value >> 10)));
    append_code_unit(out, static_cast<char16_t>(0xDC00 + (value & 0x3FF)));
}

enum class Utf8Result {
    Ok,
    NeedMore,
    Error,
};

class TokenReader {
public:
    TokenReader(const std::string *prefix, const char *data, std::size_t len, std::string *out) :
        prefix_(prefix), data_(data), len_(len), out_(out) {}

    [[nodiscard]] bool read(unsigned char &ch) {
        if (prefix_ && prefix_pos_ < prefix_->size()) {
            ch = static_cast<unsigned char>((*prefix_)[prefix_pos_++]);
        } else if (data_pos_ < len_) {
            ch = static_cast<unsigned char>(data_[data_pos_++]);
        } else {
            return false;
        }
        if (out_) {
            out_->push_back(static_cast<char>(ch));
        }
        return true;
    }

    void unread() {
        if (out_ && !out_->empty()) {
            out_->pop_back();
        }
        if (data_pos_ > 0) {
            data_pos_ -= 1;
            return;
        }
        if (prefix_pos_ > 0) {
            prefix_pos_ -= 1;
        }
    }

    [[nodiscard]] std::size_t input_consumed() const { return data_pos_; }
    [[nodiscard]] std::size_t total_consumed() const { return prefix_pos_ + data_pos_; }

private:
    const std::string *prefix_ = nullptr;
    const char *data_ = nullptr;
    std::size_t len_ = 0;
    std::string *out_ = nullptr;
    std::size_t prefix_pos_ = 0;
    std::size_t data_pos_ = 0;
};

Utf8Result scan_utf8_codepoint(TokenReader &reader, unsigned char first, bool final, ParseError &error,
                               std::size_t token_offset) {
    int needed = 0;
    std::uint32_t code = 0;
    std::uint32_t min_value = 0;
    if ((first & 0xE0) == 0xC0) {
        needed = 1;
        code = first & 0x1F;
        min_value = 0x80;
    } else if ((first & 0xF0) == 0xE0) {
        needed = 2;
        code = first & 0x0F;
        min_value = 0x800;
    } else if ((first & 0xF8) == 0xF0) {
        needed = 3;
        code = first & 0x07;
        min_value = 0x10000;
    } else {
        set_parse_error(error, "invalid utf-8 sequence", token_offset + reader.total_consumed() - 1);
        return Utf8Result::Error;
    }

    for (int idx = 0; idx < needed; ++idx) {
        unsigned char next = 0;
        if (!reader.read(next)) {
            if (!final) {
                return Utf8Result::NeedMore;
            }
            set_parse_error(error, "invalid utf-8 sequence", token_offset + reader.total_consumed());
            return Utf8Result::Error;
        }
        if ((next & 0xC0) != 0x80) {
            set_parse_error(error, "invalid utf-8 sequence", token_offset + reader.total_consumed() - 1);
            return Utf8Result::Error;
        }
        code = (code << 6) | (next & 0x3F);
    }

    if (code < min_value || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        set_parse_error(error, "invalid utf-8 sequence", token_offset + reader.total_consumed() - needed - 1);
        return Utf8Result::Error;
    }
    return Utf8Result::Ok;
}

ScanStatus scan_string(TokenReader &reader, bool final, ParseError &error, std::size_t token_offset,
                       ScannedToken &out) {
    unsigned char ch = 0;
    if (!reader.read(ch) || ch != '"') {
        set_parse_error(error, "invalid string", token_offset);
        return ScanStatus::Error;
    }

    bool has_escape = false;
    while (true) {
        if (!reader.read(ch)) {
            if (!final) {
                return ScanStatus::NeedMore;
            }
            set_parse_error(error, "unterminated string", token_offset + reader.total_consumed());
            return ScanStatus::Error;
        }
        if (ch == '"') {
            out.kind = has_escape ? TokenKind::StringEscaped : TokenKind::String;
            out.token_len = reader.total_consumed();
            out.data_offset = 1;
            out.data_len = out.token_len - 2;
            return ScanStatus::Complete;
        }
        if (ch == '\\') {
            has_escape = true;
            if (!reader.read(ch)) {
                if (!final) {
                    return ScanStatus::NeedMore;
                }
                set_parse_error(error, "unterminated escape sequence", token_offset + reader.total_consumed());
                return ScanStatus::Error;
            }
            switch (ch) {
                case '"':
                case '\\':
                case '/':
                case 'b':
                case 'f':
                case 'n':
                case 'r':
                case 't':
                    break;
                case 'u':
                    for (int idx = 0; idx < 4; ++idx) {
                        if (!reader.read(ch)) {
                            if (!final) {
                                return ScanStatus::NeedMore;
                            }
                            set_parse_error(error, "invalid unicode escape", token_offset + reader.total_consumed());
                            return ScanStatus::Error;
                        }
                        if (hex_value(static_cast<char>(ch)) < 0) {
                            set_parse_error(error, "invalid unicode escape",
                                            token_offset + reader.total_consumed() - 1);
                            return ScanStatus::Error;
                        }
                    }
                    break;
                default:
                    set_parse_error(error, "invalid escape sequence", token_offset + reader.total_consumed() - 1);
                    return ScanStatus::Error;
            }
            continue;
        }
        if (ch < 0x20) {
            set_parse_error(error, "invalid control character in string", token_offset + reader.total_consumed() - 1);
            return ScanStatus::Error;
        }
        if (ch >= 0x80) {
            Utf8Result result = scan_utf8_codepoint(reader, ch, final, error, token_offset);
            if (result == Utf8Result::NeedMore) {
                return ScanStatus::NeedMore;
            }
            if (result == Utf8Result::Error) {
                return ScanStatus::Error;
            }
        }
    }
}

ScanStatus scan_literal(TokenReader &reader, bool final, const char *literal, TokenKind kind, ParseError &error,
                        std::size_t token_offset, ScannedToken &out) {
    for (std::size_t idx = 0; literal[idx] != '\0'; ++idx) {
        unsigned char ch = 0;
        if (!reader.read(ch)) {
            if (!final) {
                return ScanStatus::NeedMore;
            }
            set_parse_error(error, "invalid literal", token_offset + reader.total_consumed());
            return ScanStatus::Error;
        }
        if (ch != static_cast<unsigned char>(literal[idx])) {
            set_parse_error(error, "invalid literal", token_offset + reader.total_consumed() - 1);
            return ScanStatus::Error;
        }
    }
    out.kind = kind;
    out.token_len = reader.total_consumed();
    out.data_offset = 0;
    out.data_len = out.token_len;
    return ScanStatus::Complete;
}

ScanStatus scan_number(TokenReader &reader, bool final, ParseError &error, std::size_t token_offset,
                       ScannedToken &out) {
    TokenKind kind = TokenKind::Integer;
    unsigned char ch = 0;
    if (!reader.read(ch)) {
        if (!final) {
            return ScanStatus::NeedMore;
        }
        set_parse_error(error, "invalid number", token_offset);
        return ScanStatus::Error;
    }

    if (ch == '-') {
        if (!reader.read(ch)) {
            if (!final) {
                return ScanStatus::NeedMore;
            }
            set_parse_error(error, "invalid number", token_offset);
            return ScanStatus::Error;
        }
    }

    if (ch == '0') {
        if (!reader.read(ch)) {
            if (!final) {
                return ScanStatus::NeedMore;
            }
            out.kind = kind;
            out.token_len = reader.total_consumed();
            out.data_offset = 0;
            out.data_len = out.token_len;
            return ScanStatus::Complete;
        }
        if (is_digit(ch)) {
            set_parse_error(error, "leading zero in number", token_offset);
            return ScanStatus::Error;
        }
    } else if (ch >= '1' && ch <= '9') {
        while (true) {
            if (!reader.read(ch)) {
                if (!final) {
                    return ScanStatus::NeedMore;
                }
                out.kind = kind;
                out.token_len = reader.total_consumed();
                out.data_offset = 0;
                out.data_len = out.token_len;
                return ScanStatus::Complete;
            }
            if (!is_digit(ch)) {
                break;
            }
        }
    } else {
        set_parse_error(error, "invalid number", token_offset);
        return ScanStatus::Error;
    }

    if (ch == '.') {
        kind = TokenKind::Double;
        if (!reader.read(ch)) {
            if (!final) {
                return ScanStatus::NeedMore;
            }
            set_parse_error(error, "invalid number", token_offset);
            return ScanStatus::Error;
        }
        if (!is_digit(ch)) {
            set_parse_error(error, "invalid number", token_offset);
            return ScanStatus::Error;
        }
        do {
            if (!reader.read(ch)) {
                if (!final) {
                    return ScanStatus::NeedMore;
                }
                out.kind = kind;
                out.token_len = reader.total_consumed();
                out.data_offset = 0;
                out.data_len = out.token_len;
                return ScanStatus::Complete;
            }
        } while (is_digit(ch));
    }

    if (ch == 'e' || ch == 'E') {
        kind = TokenKind::Double;
        if (!reader.read(ch)) {
            if (!final) {
                return ScanStatus::NeedMore;
            }
            set_parse_error(error, "invalid number", token_offset);
            return ScanStatus::Error;
        }
        if (ch == '+' || ch == '-') {
            if (!reader.read(ch)) {
                if (!final) {
                    return ScanStatus::NeedMore;
                }
                set_parse_error(error, "invalid number", token_offset);
                return ScanStatus::Error;
            }
        }
        if (!is_digit(ch)) {
            set_parse_error(error, "invalid number", token_offset);
            return ScanStatus::Error;
        }
        do {
            if (!reader.read(ch)) {
                if (!final) {
                    return ScanStatus::NeedMore;
                }
                out.kind = kind;
                out.token_len = reader.total_consumed();
                out.data_offset = 0;
                out.data_len = out.token_len;
                return ScanStatus::Complete;
            }
        } while (is_digit(ch));
    }

    if (!is_number_delimiter(ch)) {
        set_parse_error(error, "invalid number", token_offset + reader.total_consumed() - 1);
        return ScanStatus::Error;
    }

    reader.unread();
    out.kind = kind;
    out.token_len = reader.total_consumed();
    out.data_offset = 0;
    out.data_len = out.token_len;
    return ScanStatus::Complete;
}

class JsonLexer {
public:
    void reset() {
        stream_offset_ = 0;
        partial_offset_ = 0;
        partial_.clear();
        token_storage_.clear();
    }

    [[nodiscard]] std::size_t absolute_offset(std::size_t offset) const { return stream_offset_ + offset; }

    void finish_chunk(std::size_t consumed) { stream_offset_ += consumed; }

    [[nodiscard]] LexStatus next(const char *data, std::size_t len, std::size_t &offset, bool final, Token &out,
                                 ParseError &error) {
        token_storage_.clear();
        out = {};

        if (!partial_.empty()) {
            return continue_partial(data, len, offset, final, out, error);
        }

        while (offset < len && is_ws(data[offset])) {
            offset += 1;
        }
        if (offset >= len) {
            out.kind = TokenKind::Eof;
            out.offset = stream_offset_ + offset;
            return final ? LexStatus::Ok : LexStatus::NeedMore;
        }

        std::size_t start = offset;
        std::size_t token_offset = stream_offset_ + start;
        unsigned char ch = static_cast<unsigned char>(data[offset]);
        switch (ch) {
            case '{':
                return single_char(TokenKind::ObjectOpen, data, offset, out);
            case '}':
                return single_char(TokenKind::ObjectClose, data, offset, out);
            case '[':
                return single_char(TokenKind::ArrayOpen, data, offset, out);
            case ']':
                return single_char(TokenKind::ArrayClose, data, offset, out);
            case ':':
                return single_char(TokenKind::Colon, data, offset, out);
            case ',':
                return single_char(TokenKind::Comma, data, offset, out);
            case '"':
                return scan_current(data, len, offset, final, token_offset, out, error, scan_string);
            case 't':
                return scan_current_literal(data, len, offset, final, token_offset, "true", TokenKind::Bool, out,
                                            error);
            case 'f':
                return scan_current_literal(data, len, offset, final, token_offset, "false", TokenKind::Bool, out,
                                            error);
            case 'n':
                return scan_current_literal(data, len, offset, final, token_offset, "null", TokenKind::Null, out,
                                            error);
            default:
                if (ch == '-' || is_digit(ch)) {
                    return scan_current(data, len, offset, final, token_offset, out, error, scan_number);
                }
                set_parse_error(error, "invalid token", token_offset);
                return LexStatus::Error;
        }
    }

private:
    using Scanner = ScanStatus (*)(TokenReader &, bool, ParseError &, std::size_t, ScannedToken &);

    [[nodiscard]] LexStatus single_char(TokenKind kind, const char *data, std::size_t &offset, Token &out) const {
        out.kind = kind;
        out.data = data + offset;
        out.len = 1;
        out.offset = stream_offset_ + offset;
        offset += 1;
        return LexStatus::Ok;
    }

    [[nodiscard]] LexStatus scan_current(const char *data, std::size_t len, std::size_t &offset, bool final,
                                         std::size_t token_offset, Token &out, ParseError &error, Scanner scanner) {
        std::size_t start = offset;
        TokenReader reader(nullptr, data + start, len - start, nullptr);
        ScannedToken scanned;
        ScanStatus status = scanner(reader, final, error, token_offset, scanned);
        if (status == ScanStatus::NeedMore) {
            partial_.assign(data + start, len - start);
            partial_offset_ = token_offset;
            offset = len;
            return LexStatus::NeedMore;
        }
        if (status == ScanStatus::Error) {
            offset = start + reader.input_consumed();
            return LexStatus::Error;
        }
        out.kind = scanned.kind;
        out.data = data + start + scanned.data_offset;
        out.len = scanned.data_len;
        out.offset = token_offset;
        offset = start + scanned.token_len;
        return LexStatus::Ok;
    }

    [[nodiscard]] LexStatus scan_current_literal(const char *data, std::size_t len, std::size_t &offset, bool final,
                                                 std::size_t token_offset, const char *literal, TokenKind kind,
                                                 Token &out, ParseError &error) {
        std::size_t start = offset;
        TokenReader reader(nullptr, data + start, len - start, nullptr);
        ScannedToken scanned;
        ScanStatus status = scan_literal(reader, final, literal, kind, error, token_offset, scanned);
        if (status == ScanStatus::NeedMore) {
            partial_.assign(data + start, len - start);
            partial_offset_ = token_offset;
            offset = len;
            return LexStatus::NeedMore;
        }
        if (status == ScanStatus::Error) {
            offset = start + reader.input_consumed();
            return LexStatus::Error;
        }
        out.kind = scanned.kind;
        out.data = data + start + scanned.data_offset;
        out.len = scanned.data_len;
        out.offset = token_offset;
        offset = start + scanned.token_len;
        return LexStatus::Ok;
    }

    [[nodiscard]] LexStatus continue_partial(const char *data, std::size_t len, std::size_t &offset, bool final,
                                             Token &out, ParseError &error) {
        TokenReader reader(&partial_, data + offset, len - offset, &token_storage_);
        ScannedToken scanned;
        ScanStatus status = ScanStatus::Error;
        unsigned char first = static_cast<unsigned char>(partial_[0]);
        if (first == '"') {
            status = scan_string(reader, final, error, partial_offset_, scanned);
        } else if (first == 't') {
            status = scan_literal(reader, final, "true", TokenKind::Bool, error, partial_offset_, scanned);
        } else if (first == 'f') {
            status = scan_literal(reader, final, "false", TokenKind::Bool, error, partial_offset_, scanned);
        } else if (first == 'n') {
            status = scan_literal(reader, final, "null", TokenKind::Null, error, partial_offset_, scanned);
        } else {
            status = scan_number(reader, final, error, partial_offset_, scanned);
        }

        offset += reader.input_consumed();
        if (status == ScanStatus::NeedMore) {
            partial_ = token_storage_;
            token_storage_.clear();
            return LexStatus::NeedMore;
        }
        if (status == ScanStatus::Error) {
            return LexStatus::Error;
        }

        out.kind = scanned.kind;
        out.data = token_storage_.data() + scanned.data_offset;
        out.len = scanned.data_len;
        out.offset = partial_offset_;
        partial_.clear();
        partial_offset_ = 0;
        return LexStatus::Ok;
    }

    std::size_t stream_offset_ = 0;
    std::size_t partial_offset_ = 0;
    std::string partial_;
    std::string token_storage_;
};

Utf8Result decode_utf8_codepoint(const char *data, std::size_t len, std::size_t &pos, std::uint32_t &codepoint,
                                 ParseError &error, std::size_t offset_base) {
    if (pos >= len) {
        set_parse_error(error, "invalid utf-8 sequence", offset_base + pos);
        return Utf8Result::Error;
    }
    unsigned char ch = static_cast<unsigned char>(data[pos]);
    if (ch < 0x80) {
        codepoint = ch;
        pos += 1;
        return Utf8Result::Ok;
    }

    int needed = 0;
    std::uint32_t code = 0;
    std::uint32_t min_value = 0;
    if ((ch & 0xE0) == 0xC0) {
        needed = 1;
        code = ch & 0x1F;
        min_value = 0x80;
    } else if ((ch & 0xF0) == 0xE0) {
        needed = 2;
        code = ch & 0x0F;
        min_value = 0x800;
    } else if ((ch & 0xF8) == 0xF0) {
        needed = 3;
        code = ch & 0x07;
        min_value = 0x10000;
    } else {
        set_parse_error(error, "invalid utf-8 sequence", offset_base + pos);
        return Utf8Result::Error;
    }
    if (pos + static_cast<std::size_t>(needed) >= len) {
        set_parse_error(error, "invalid utf-8 sequence", offset_base + pos);
        return Utf8Result::Error;
    }
    for (int idx = 1; idx <= needed; ++idx) {
        unsigned char next = static_cast<unsigned char>(data[pos + idx]);
        if ((next & 0xC0) != 0x80) {
            set_parse_error(error, "invalid utf-8 sequence", offset_base + pos + idx);
            return Utf8Result::Error;
        }
        code = (code << 6) | (next & 0x3F);
    }
    if (code < min_value || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        set_parse_error(error, "invalid utf-8 sequence", offset_base + pos);
        return Utf8Result::Error;
    }
    pos += static_cast<std::size_t>(needed) + 1;
    codepoint = code;
    return Utf8Result::Ok;
}

bool decode_raw_string(const char *data, std::size_t len, DecodedString &out, ParseError &error, std::size_t offset) {
    out.clear();
    std::size_t pos = 0;
    while (pos < len) {
        std::uint32_t codepoint = 0;
        if (decode_utf8_codepoint(data, len, pos, codepoint, error, offset) != Utf8Result::Ok) {
            return false;
        }
        if (codepoint < 0x20) {
            return set_parse_error(error, "invalid control character in string", offset + pos);
        }
        append_codepoint(out, codepoint);
    }
    return true;
}

bool decode_escaped_string(const char *data, std::size_t len, DecodedString &out, ParseError &error,
                           std::size_t offset) {
    out.clear();
    std::size_t pos = 0;
    while (pos < len) {
        unsigned char ch = static_cast<unsigned char>(data[pos]);
        if (ch == '\\') {
            pos += 1;
            if (pos >= len) {
                return set_parse_error(error, "unterminated escape sequence", offset + pos);
            }
            char esc = data[pos++];
            switch (esc) {
                case '"':
                    append_code_unit(out, '"');
                    break;
                case '\\':
                    append_code_unit(out, '\\');
                    break;
                case '/':
                    append_code_unit(out, '/');
                    break;
                case 'b':
                    append_code_unit(out, '\b');
                    break;
                case 'f':
                    append_code_unit(out, '\f');
                    break;
                case 'n':
                    append_code_unit(out, '\n');
                    break;
                case 'r':
                    append_code_unit(out, '\r');
                    break;
                case 't':
                    append_code_unit(out, '\t');
                    break;
                case 'u': {
                    if (pos + 4 > len) {
                        return set_parse_error(error, "invalid unicode escape", offset + pos);
                    }
                    std::uint32_t code = 0;
                    for (int idx = 0; idx < 4; ++idx) {
                        int digit = hex_value(data[pos + idx]);
                        if (digit < 0) {
                            return set_parse_error(error, "invalid unicode escape", offset + pos + idx);
                        }
                        code = (code << 4) | static_cast<std::uint32_t>(digit);
                    }
                    pos += 4;
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (pos + 6 > len || data[pos] != '\\' || data[pos + 1] != 'u') {
                            return set_parse_error(error, "invalid unicode surrogate pair", offset + pos);
                        }
                        pos += 2;
                        std::uint32_t low = 0;
                        for (int idx = 0; idx < 4; ++idx) {
                            int digit = hex_value(data[pos + idx]);
                            if (digit < 0) {
                                return set_parse_error(error, "invalid unicode escape", offset + pos + idx);
                            }
                            low = (low << 4) | static_cast<std::uint32_t>(digit);
                        }
                        pos += 4;
                        if (low < 0xDC00 || low > 0xDFFF) {
                            return set_parse_error(error, "invalid unicode surrogate pair", offset + pos);
                        }
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        return set_parse_error(error, "invalid unicode surrogate pair", offset + pos);
                    }
                    append_codepoint(out, code);
                    break;
                }
                default:
                    return set_parse_error(error, "invalid escape sequence", offset + pos - 1);
            }
            continue;
        }
        if (ch < 0x20) {
            return set_parse_error(error, "invalid control character in string", offset + pos);
        }
        std::uint32_t codepoint = 0;
        if (decode_utf8_codepoint(data, len, pos, codepoint, error, offset) != Utf8Result::Ok) {
            return false;
        }
        append_codepoint(out, codepoint);
    }
    return true;
}

bool decode_json_string(TokenKind kind, const char *data, std::size_t len, DecodedString &out, ParseError &error,
                        std::size_t offset) {
    if (kind == TokenKind::StringEscaped) {
        return decode_escaped_string(data, len, out, error, offset);
    }
    return decode_raw_string(data, len, out, error, offset);
}

GcString *make_gc_string(GcHeap &heap, const DecodedString &decoded) {
    if (decoded.is_byte) {
        return gc_new_string_bytes(&heap, decoded.bytes.data(), decoded.bytes.size());
    }
    return gc_new_string_utf16(&heap, decoded.u16.data(), decoded.u16.size());
}

class JsonEventSink {
public:
    virtual ~JsonEventSink() = default;
    [[nodiscard]] virtual bool null_value(std::size_t offset) = 0;
    [[nodiscard]] virtual bool bool_value(bool value, std::size_t offset) = 0;
    [[nodiscard]] virtual bool number_value(TokenKind kind, const char *data, std::size_t len, std::size_t offset) = 0;
    [[nodiscard]] virtual bool string_value(TokenKind kind, const char *data, std::size_t len, std::size_t offset) = 0;
    [[nodiscard]] virtual bool object_key(TokenKind kind, const char *data, std::size_t len, std::size_t offset) = 0;
    [[nodiscard]] virtual bool start_object(std::size_t offset) = 0;
    [[nodiscard]] virtual bool end_object(std::size_t offset) = 0;
    [[nodiscard]] virtual bool start_array(std::size_t offset) = 0;
    [[nodiscard]] virtual bool end_array(std::size_t offset) = 0;
};

class JsValueSink final : public JsonEventSink {
public:
    JsValueSink(GcHeap &heap, ParseError &error) : heap_(heap), error_(error) {}

    void reset() {
        root_ = JsValue();
        has_result_ = false;
        containers_.clear();
    }

    [[nodiscard]] const JsValue &root() const { return root_; }
    [[nodiscard]] bool has_result() const { return has_result_; }

    [[nodiscard]] bool null_value(std::size_t offset) override { return add_value(JsValue::make_null(), offset); }

    [[nodiscard]] bool bool_value(bool value, std::size_t offset) override {
        return add_value(JsValue::make_boolean(value), offset);
    }

    [[nodiscard]] bool number_value(TokenKind kind, const char *data, std::size_t len, std::size_t offset) override {
        if (kind == TokenKind::Integer) {
            std::int64_t value = 0;
            auto result = std::from_chars(data, data + len, value);
            if (result.ec == std::errc::result_out_of_range) {
                return set_parse_error(error_, "integer overflow", offset);
            }
            if (result.ec != std::errc() || result.ptr != data + len) {
                return set_parse_error(error_, "invalid number", offset);
            }
            return add_value(JsValue::make_integer(value), offset);
        }

        double value = 0.0;
        auto result = std::from_chars(data, data + len, value);
        if (result.ec == std::errc::result_out_of_range || !std::isfinite(value)) {
            return set_parse_error(error_, "floating point overflow", offset);
        }
        if (result.ec != std::errc() || result.ptr != data + len) {
            return set_parse_error(error_, "invalid number", offset);
        }
        return add_value(JsValue::make_float(value), offset);
    }

    [[nodiscard]] bool string_value(TokenKind kind, const char *data, std::size_t len, std::size_t offset) override {
        DecodedString decoded;
        if (!decode_json_string(kind, data, len, decoded, error_, offset)) {
            return false;
        }
        GcString *str = make_gc_string(heap_, decoded);
        if (!str) {
            return set_parse_error(error_, "out of memory", offset);
        }
        return add_value(js_make_heap_ref(&str->hdr, JsHeapKind::String), offset);
    }

    [[nodiscard]] bool object_key(TokenKind kind, const char *data, std::size_t len, std::size_t offset) override {
        if (containers_.empty() || containers_.back().type != JsNodeType::Object) {
            return set_parse_error(error_, "invalid object state", offset);
        }
        ContainerFrame &frame = containers_.back();
        if (frame.has_key) {
            return set_parse_error(error_, "object key already pending", offset);
        }
        if (!decode_json_string(kind, data, len, frame.key, error_, offset)) {
            return false;
        }
        frame.has_key = true;
        return true;
    }

    [[nodiscard]] bool start_object(std::size_t offset) override {
        GcObject *obj = gc_new_object(&heap_, kInitialContainerCapacity);
        if (!obj) {
            return set_parse_error(error_, "out of memory", offset);
        }
        if (!add_value(js_make_heap_ref(&obj->hdr, JsHeapKind::Object), offset)) {
            return false;
        }
        ContainerFrame frame;
        frame.type = JsNodeType::Object;
        frame.object = obj;
        containers_.push_back(std::move(frame));
        return true;
    }

    [[nodiscard]] bool end_object(std::size_t offset) override {
        if (containers_.empty() || containers_.back().type != JsNodeType::Object) {
            return set_parse_error(error_, "mismatched object close", offset);
        }
        if (containers_.back().has_key) {
            return set_parse_error(error_, "object key missing value", offset);
        }
        containers_.pop_back();
        return true;
    }

    [[nodiscard]] bool start_array(std::size_t offset) override {
        GcArray *arr = gc_new_array(&heap_, kInitialContainerCapacity);
        if (!arr) {
            return set_parse_error(error_, "out of memory", offset);
        }
        if (!add_value(js_make_heap_ref(&arr->hdr, JsHeapKind::Array), offset)) {
            return false;
        }
        ContainerFrame frame;
        frame.type = JsNodeType::Array;
        frame.array = arr;
        containers_.push_back(std::move(frame));
        return true;
    }

    [[nodiscard]] bool end_array(std::size_t offset) override {
        if (containers_.empty() || containers_.back().type != JsNodeType::Array) {
            return set_parse_error(error_, "mismatched array close", offset);
        }
        containers_.pop_back();
        return true;
    }

private:
    struct ContainerFrame {
        JsNodeType type = JsNodeType::Undefined;
        GcArray *array = nullptr;
        GcObject *object = nullptr;
        DecodedString key;
        bool has_key = false;
    };

    [[nodiscard]] bool add_value(JsValue value, std::size_t offset) {
        if (containers_.empty()) {
            if (has_result_) {
                return set_parse_error(error_, "multiple top-level values", offset);
            }
            root_ = std::move(value);
            has_result_ = true;
            return true;
        }

        ContainerFrame &frame = containers_.back();
        if (frame.type == JsNodeType::Array) {
            if (!gc_array_push(&heap_, frame.array, std::move(value))) {
                return set_parse_error(error_, "out of memory", offset);
            }
            return true;
        }
        if (frame.type == JsNodeType::Object) {
            if (!frame.has_key) {
                return set_parse_error(error_, "object value missing key", offset);
            }
            GcString *key = make_gc_string(heap_, frame.key);
            if (!key) {
                return set_parse_error(error_, "out of memory", offset);
            }
            if (!gc_object_set(&heap_, frame.object, key, std::move(value))) {
                return set_parse_error(error_, "out of memory", offset);
            }
            frame.key.clear();
            frame.has_key = false;
            return true;
        }
        return set_parse_error(error_, "invalid container state", offset);
    }

    GcHeap &heap_;
    ParseError &error_;
    JsValue root_;
    bool has_result_ = false;
    std::vector<ContainerFrame> containers_;
};

class JsonParserCore {
public:
    void reset() {
        lexer_.reset();
        state_stack_.clear();
        state_stack_.push_back(ParseState::Start);
    }

    [[nodiscard]] StreamParser::Status parse(const char *data, std::size_t len, bool final, JsonEventSink &sink,
                                             ParseError &error) {
        std::size_t offset = 0;
        while (true) {
            Token tok;
            LexStatus lex_status = lexer_.next(data, len, offset, final, tok, error);
            if (lex_status == LexStatus::NeedMore) {
                lexer_.finish_chunk(offset);
                if (current_state() == ParseState::ParseComplete) {
                    return StreamParser::Status::Complete;
                }
                return StreamParser::Status::NeedMore;
            }
            if (lex_status == LexStatus::Error) {
                set_state(ParseState::ParseError);
                return StreamParser::Status::Error;
            }
            if (tok.kind == TokenKind::Eof) {
                lexer_.finish_chunk(offset);
                if (current_state() == ParseState::ParseComplete) {
                    return StreamParser::Status::Complete;
                }
                if (final) {
                    set_parse_error(error, "premature EOF", tok.offset);
                    set_state(ParseState::ParseError);
                    return StreamParser::Status::Error;
                }
                return StreamParser::Status::NeedMore;
            }
            if (current_state() == ParseState::ParseComplete) {
                set_parse_error(error, "trailing garbage after JSON value", tok.offset);
                set_state(ParseState::ParseError);
                return StreamParser::Status::Error;
            }
            if (!process_token(tok, sink, error)) {
                set_state(ParseState::ParseError);
                return StreamParser::Status::Error;
            }
        }
    }

private:
    [[nodiscard]] ParseState current_state() const { return state_stack_.back(); }

    void set_state(ParseState state) { state_stack_.back() = state; }

    [[nodiscard]] bool can_accept_value() const {
        ParseState state = current_state();
        return state == ParseState::Start || state == ParseState::ObjectNeedVal || state == ParseState::ArrayNeedVal ||
               state == ParseState::ArrayStart;
    }

    [[nodiscard]] bool mark_value_started(std::size_t offset, ParseError &error) {
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
                return set_parse_error(error, "unexpected value", offset);
        }
    }

    [[nodiscard]] bool finish_sink_event(bool ok, ParseError &error, std::size_t offset) const {
        if (ok) {
            return true;
        }
        return set_parse_error(error, "event sink rejected token", offset);
    }

    [[nodiscard]] bool process_value_token(const Token &tok, JsonEventSink &sink, ParseError &error) {
        if (!can_accept_value()) {
            return set_parse_error(error, "unexpected token", tok.offset);
        }

        switch (tok.kind) {
            case TokenKind::Null:
                if (!finish_sink_event(sink.null_value(tok.offset), error, tok.offset)) {
                    return false;
                }
                return mark_value_started(tok.offset, error);
            case TokenKind::Bool:
                if (!finish_sink_event(sink.bool_value(tok.len > 0 && tok.data[0] == 't', tok.offset), error,
                                       tok.offset)) {
                    return false;
                }
                return mark_value_started(tok.offset, error);
            case TokenKind::Integer:
            case TokenKind::Double:
                if (!finish_sink_event(sink.number_value(tok.kind, tok.data, tok.len, tok.offset), error, tok.offset)) {
                    return false;
                }
                return mark_value_started(tok.offset, error);
            case TokenKind::String:
            case TokenKind::StringEscaped:
                if (!finish_sink_event(sink.string_value(tok.kind, tok.data, tok.len, tok.offset), error, tok.offset)) {
                    return false;
                }
                return mark_value_started(tok.offset, error);
            case TokenKind::ObjectOpen:
                if (!finish_sink_event(sink.start_object(tok.offset), error, tok.offset)) {
                    return false;
                }
                if (!mark_value_started(tok.offset, error)) {
                    return false;
                }
                state_stack_.push_back(ParseState::ObjectStart);
                return true;
            case TokenKind::ArrayOpen:
                if (!finish_sink_event(sink.start_array(tok.offset), error, tok.offset)) {
                    return false;
                }
                if (!mark_value_started(tok.offset, error)) {
                    return false;
                }
                state_stack_.push_back(ParseState::ArrayStart);
                return true;
            default:
                return set_parse_error(error, "unallowed token at this point in JSON text", tok.offset);
        }
    }

    [[nodiscard]] bool close_container(TokenKind kind, JsonEventSink &sink, ParseError &error, std::size_t offset) {
        bool ok = false;
        if (kind == TokenKind::ObjectClose) {
            ok = sink.end_object(offset);
        } else {
            ok = sink.end_array(offset);
        }
        if (!finish_sink_event(ok, error, offset)) {
            return false;
        }
        if (state_stack_.size() <= 1) {
            return set_parse_error(error, "invalid parser state", offset);
        }
        state_stack_.pop_back();
        return true;
    }

    [[nodiscard]] bool process_token(const Token &tok, JsonEventSink &sink, ParseError &error) {
        switch (current_state()) {
            case ParseState::ObjectStart:
            case ParseState::ObjectNeedKey:
                if (tok.kind == TokenKind::ObjectClose && current_state() == ParseState::ObjectStart) {
                    return close_container(tok.kind, sink, error, tok.offset);
                }
                if (tok.kind != TokenKind::String && tok.kind != TokenKind::StringEscaped) {
                    return set_parse_error(error, "object key must be a string", tok.offset);
                }
                if (!finish_sink_event(sink.object_key(tok.kind, tok.data, tok.len, tok.offset), error, tok.offset)) {
                    return false;
                }
                set_state(ParseState::ObjectSep);
                return true;
            case ParseState::ObjectSep:
                if (tok.kind != TokenKind::Colon) {
                    return set_parse_error(error, "expected ':' after object key", tok.offset);
                }
                set_state(ParseState::ObjectNeedVal);
                return true;
            case ParseState::ObjectGotVal:
                if (tok.kind == TokenKind::ObjectClose) {
                    return close_container(tok.kind, sink, error, tok.offset);
                }
                if (tok.kind == TokenKind::Comma) {
                    set_state(ParseState::ObjectNeedKey);
                    return true;
                }
                return set_parse_error(error, "after object value, expected ',' or '}'", tok.offset);
            case ParseState::ArrayStart:
                if (tok.kind == TokenKind::ArrayClose) {
                    return close_container(tok.kind, sink, error, tok.offset);
                }
                return process_value_token(tok, sink, error);
            case ParseState::ArrayGotVal:
                if (tok.kind == TokenKind::ArrayClose) {
                    return close_container(tok.kind, sink, error, tok.offset);
                }
                if (tok.kind == TokenKind::Comma) {
                    set_state(ParseState::ArrayNeedVal);
                    return true;
                }
                return set_parse_error(error, "after array value, expected ',' or ']'", tok.offset);
            case ParseState::Start:
            case ParseState::ObjectNeedVal:
            case ParseState::ArrayNeedVal:
                return process_value_token(tok, sink, error);
            case ParseState::ParseComplete:
                return set_parse_error(error, "trailing garbage after JSON value", tok.offset);
            case ParseState::ParseError:
                return set_parse_error(error, "invalid parser state", tok.offset);
        }
        return set_parse_error(error, "invalid parser state", tok.offset);
    }

    JsonLexer lexer_;
    std::vector<ParseState> state_stack_{ParseState::Start};
};

} // namespace

struct StreamParser::Impl {
    explicit Impl(GcHeap &heap) : heap(heap), sink(heap, error) { reset(); }

    void reset() {
        error = {};
        core.reset();
        sink.reset();
    }

    [[nodiscard]] Status parse(const char *data, std::size_t len, bool final) {
        if (!data && len > 0) {
            set_parse_error(error, "input is null", 0);
            return Status::Error;
        }
        return core.parse(data ? data : "", len, final, sink, error);
    }

    GcHeap &heap;
    ParseError error;
    JsonParserCore core;
    JsValueSink sink;
};

Parser::Parser(GcHeap &heap) : heap_(heap) {}

bool Parser::parse(const char *data, std::size_t len, JsValue &out) {
    error_ = {};
    if (!data && len > 0) {
        error_.message = "input is null";
        error_.offset = 0;
        return false;
    }

    JsonParserCore core;
    JsValueSink sink(heap_, error_);
    StreamParser::Status status = core.parse(data ? data : "", len, true, sink, error_);
    if (status != StreamParser::Status::Complete || !sink.has_result()) {
        if (error_.message.empty()) {
            error_.message = "no JSON value";
            error_.offset = 0;
        }
        return false;
    }
    out = sink.root();
    return true;
}

bool Parser::parse(const std::string &data, JsValue &out) { return parse(data.data(), data.size(), out); }

const ParseError &Parser::error() const { return error_; }

StreamParser::StreamParser(GcHeap &heap) : impl_(std::make_unique<Impl>(heap)) {}

StreamParser::~StreamParser() = default;

void StreamParser::reset() { impl_->reset(); }

StreamParser::Status StreamParser::parse(const char *data, std::size_t len) { return impl_->parse(data, len, false); }

StreamParser::Status StreamParser::finish() { return impl_->parse("", 0, true); }

const ParseError &StreamParser::error() const { return impl_->error; }

const JsValue &StreamParser::root() const { return impl_->sink.root(); }

bool StreamParser::has_result() const { return impl_->sink.has_result(); }

} // namespace fiber::json
