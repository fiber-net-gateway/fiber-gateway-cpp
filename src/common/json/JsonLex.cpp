#include <fiber/common/json/JsonLex.h>

#include <cstdlib>
#include <cstring>
#include <limits>

namespace fiber::json::detail {
namespace {

bool set_parse_error(ParseError &error, const char *message, std::size_t offset) noexcept {
    if (!error.message) {
        error.message = message;
        error.offset = offset;
    }
    return false;
}

bool is_ws(char ch) noexcept { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; }

bool is_digit(unsigned char ch) noexcept { return ch >= '0' && ch <= '9'; }

bool is_number_delimiter(unsigned char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == ',' || ch == ']' || ch == '}';
}

int hex_value(char ch) noexcept {
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

bool append_utf8(Buffer &scratch, std::uint32_t codepoint, ParseError &error, std::size_t offset) noexcept {
    if (codepoint <= 0x7F) {
        if (!scratch.append(static_cast<char>(codepoint))) {
            return set_parse_error(error, "out of memory", offset);
        }
        return true;
    }
    if (codepoint <= 0x7FF) {
        char bytes[2] = {
                static_cast<char>(0xC0 | (codepoint >> 6)),
                static_cast<char>(0x80 | (codepoint & 0x3F)),
        };
        if (!scratch.append(bytes, sizeof(bytes))) {
            return set_parse_error(error, "out of memory", offset);
        }
        return true;
    }
    if (codepoint <= 0xFFFF) {
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            return set_parse_error(error, "invalid unicode surrogate pair", offset);
        }
        char bytes[3] = {
                static_cast<char>(0xE0 | (codepoint >> 12)),
                static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)),
                static_cast<char>(0x80 | (codepoint & 0x3F)),
        };
        if (!scratch.append(bytes, sizeof(bytes))) {
            return set_parse_error(error, "out of memory", offset);
        }
        return true;
    }
    if (codepoint <= 0x10FFFF) {
        char bytes[4] = {
                static_cast<char>(0xF0 | (codepoint >> 18)),
                static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)),
                static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)),
                static_cast<char>(0x80 | (codepoint & 0x3F)),
        };
        if (!scratch.append(bytes, sizeof(bytes))) {
            return set_parse_error(error, "out of memory", offset);
        }
        return true;
    }
    return set_parse_error(error, "invalid unicode escape", offset);
}

} // namespace

void Buffer::clear() noexcept { size = 0; }

void Buffer::reset() noexcept {
    std::free(data);
    data = nullptr;
    size = 0;
    capacity = 0;
}

bool Buffer::reserve(std::size_t needed) noexcept {
    if (needed <= capacity) {
        return true;
    }

    std::size_t next = capacity == 0 ? 64 : capacity;
    while (next < needed) {
        if (next > std::numeric_limits<std::size_t>::max() / 2) {
            next = needed;
            break;
        }
        next *= 2;
    }

    void *raw = std::realloc(data, next);
    if (!raw) {
        return false;
    }
    data = static_cast<char *>(raw);
    capacity = next;
    return true;
}

bool Buffer::append(char ch) noexcept {
    if (size == std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    if (!reserve(size + 1)) {
        return false;
    }
    data[size++] = ch;
    return true;
}

bool Buffer::append(const char *src, std::size_t len) noexcept {
    if (len == 0) {
        return true;
    }
    if (!src || len > std::numeric_limits<std::size_t>::max() - size) {
        return false;
    }
    if (!reserve(size + len)) {
        return false;
    }
    std::memcpy(data + size, src, len);
    size += len;
    return true;
}

bool Lexer::TokenReader::read(unsigned char &ch) noexcept {
    if (prefix && prefix_pos < prefix->size) {
        ch = static_cast<unsigned char>(prefix->data[prefix_pos++]);
    } else if (data_pos < len) {
        ch = static_cast<unsigned char>(data[data_pos++]);
    } else {
        return false;
    }

    if (out && !out->append(static_cast<char>(ch))) {
        failed = true;
        return false;
    }
    return true;
}

void Lexer::TokenReader::unread() noexcept {
    if (out && out->size > 0) {
        out->size -= 1;
    }
    if (data_pos > 0) {
        data_pos -= 1;
        return;
    }
    if (prefix_pos > 0) {
        prefix_pos -= 1;
    }
}

std::size_t Lexer::TokenReader::input_consumed() const noexcept { return data_pos; }

std::size_t Lexer::TokenReader::total_consumed() const noexcept { return prefix_pos + data_pos; }

Lexer::~Lexer() noexcept {
    partial_.reset();
    token_storage_.reset();
}

void Lexer::reset() noexcept {
    stream_offset_ = 0;
    last_bytes_consumed_ = 0;
    partial_offset_ = 0;
    partial_.clear();
    token_storage_.clear();
}

void Lexer::finish_chunk(std::size_t consumed) noexcept {
    stream_offset_ += consumed;
    last_bytes_consumed_ = consumed;
}

std::size_t Lexer::bytes_consumed() const noexcept { return last_bytes_consumed_; }

std::size_t Lexer::stream_offset() const noexcept { return stream_offset_; }

Lexer::Utf8Status Lexer::scan_utf8_codepoint(TokenReader &reader, unsigned char first, bool final, ParseError &error,
                                             std::size_t token_offset) noexcept {
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
        return Utf8Status::Error;
    }

    for (int idx = 0; idx < needed; ++idx) {
        unsigned char next = 0;
        if (!reader.read(next)) {
            if (reader.failed) {
                set_parse_error(error, "out of memory", token_offset + reader.total_consumed());
                return Utf8Status::Error;
            }
            if (!final) {
                return Utf8Status::NeedMore;
            }
            set_parse_error(error, "invalid utf-8 sequence", token_offset + reader.total_consumed());
            return Utf8Status::Error;
        }
        if ((next & 0xC0) != 0x80) {
            set_parse_error(error, "invalid utf-8 sequence", token_offset + reader.total_consumed() - 1);
            return Utf8Status::Error;
        }
        code = (code << 6) | (next & 0x3F);
    }

    if (code < min_value || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        set_parse_error(error, "invalid utf-8 sequence", token_offset + reader.total_consumed() - needed - 1);
        return Utf8Status::Error;
    }
    return Utf8Status::Ok;
}

Lexer::ScanStatus Lexer::scan_string(TokenReader &reader, bool final, ParseError &error, std::size_t token_offset,
                                     ScannedToken &out) noexcept {
    unsigned char ch = 0;
    if (!reader.read(ch) || ch != '"') {
        if (reader.failed) {
            set_parse_error(error, "out of memory", token_offset);
        } else {
            set_parse_error(error, "invalid string", token_offset);
        }
        return ScanStatus::Error;
    }

    bool has_escape = false;
    while (true) {
        if (!reader.read(ch)) {
            if (reader.failed) {
                set_parse_error(error, "out of memory", token_offset + reader.total_consumed());
                return ScanStatus::Error;
            }
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
                if (reader.failed) {
                    set_parse_error(error, "out of memory", token_offset + reader.total_consumed());
                    return ScanStatus::Error;
                }
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
                            if (reader.failed) {
                                set_parse_error(error, "out of memory", token_offset + reader.total_consumed());
                                return ScanStatus::Error;
                            }
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
            Utf8Status result = scan_utf8_codepoint(reader, ch, final, error, token_offset);
            if (result == Utf8Status::NeedMore) {
                return ScanStatus::NeedMore;
            }
            if (result == Utf8Status::Error) {
                return ScanStatus::Error;
            }
        }
    }
}

Lexer::ScanStatus Lexer::scan_literal(TokenReader &reader, bool final, ParseError &error, std::size_t token_offset,
                                      const char *literal, TokenKind kind, ScannedToken &out) noexcept {
    for (std::size_t idx = 0; literal[idx] != '\0'; ++idx) {
        unsigned char ch = 0;
        if (!reader.read(ch)) {
            if (reader.failed) {
                set_parse_error(error, "out of memory", token_offset + reader.total_consumed());
                return ScanStatus::Error;
            }
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

Lexer::ScanStatus Lexer::scan_number(TokenReader &reader, bool final, ParseError &error, std::size_t token_offset,
                                     ScannedToken &out) noexcept {
    TokenKind kind = TokenKind::Integer;
    unsigned char ch = 0;
    if (!reader.read(ch)) {
        if (reader.failed) {
            set_parse_error(error, "out of memory", token_offset);
            return ScanStatus::Error;
        }
        if (!final) {
            return ScanStatus::NeedMore;
        }
        set_parse_error(error, "invalid number", token_offset);
        return ScanStatus::Error;
    }

    if (ch == '-') {
        if (!reader.read(ch)) {
            if (reader.failed) {
                set_parse_error(error, "out of memory", token_offset + reader.total_consumed());
                return ScanStatus::Error;
            }
            if (!final) {
                return ScanStatus::NeedMore;
            }
            set_parse_error(error, "invalid number", token_offset);
            return ScanStatus::Error;
        }
    }

    if (ch == '0') {
        if (!reader.read(ch)) {
            if (reader.failed) {
                set_parse_error(error, "out of memory", token_offset + reader.total_consumed());
                return ScanStatus::Error;
            }
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
                if (reader.failed) {
                    set_parse_error(error, "out of memory", token_offset + reader.total_consumed());
                    return ScanStatus::Error;
                }
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
            if (reader.failed) {
                set_parse_error(error, "out of memory", token_offset + reader.total_consumed());
                return ScanStatus::Error;
            }
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
                if (reader.failed) {
                    set_parse_error(error, "out of memory", token_offset + reader.total_consumed());
                    return ScanStatus::Error;
                }
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
            if (reader.failed) {
                set_parse_error(error, "out of memory", token_offset + reader.total_consumed());
                return ScanStatus::Error;
            }
            if (!final) {
                return ScanStatus::NeedMore;
            }
            set_parse_error(error, "invalid number", token_offset);
            return ScanStatus::Error;
        }
        if (ch == '+' || ch == '-') {
            if (!reader.read(ch)) {
                if (reader.failed) {
                    set_parse_error(error, "out of memory", token_offset + reader.total_consumed());
                    return ScanStatus::Error;
                }
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
                if (reader.failed) {
                    set_parse_error(error, "out of memory", token_offset + reader.total_consumed());
                    return ScanStatus::Error;
                }
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

LexStatus Lexer::scan_current(const char *data, std::size_t len, std::size_t &offset, bool final,
                              std::size_t token_offset, Token &out, ParseError &error,
                              ScanStatus (*scanner)(TokenReader &, bool, ParseError &, std::size_t,
                                                    ScannedToken &) noexcept) noexcept {
    const std::size_t start = offset;
    TokenReader reader{nullptr, data + start, len - start, nullptr};
    ScannedToken scanned;
    ScanStatus status = scanner(reader, final, error, token_offset, scanned);
    if (status == ScanStatus::NeedMore) {
        partial_.clear();
        if (!partial_.append(data + start, len - start)) {
            set_parse_error(error, "out of memory", token_offset);
            offset = len;
            return LexStatus::Error;
        }
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

LexStatus Lexer::scan_current_literal(const char *data, std::size_t len, std::size_t &offset, bool final,
                                      std::size_t token_offset, const char *literal, TokenKind kind, Token &out,
                                      ParseError &error) noexcept {
    const std::size_t start = offset;
    TokenReader reader{nullptr, data + start, len - start, nullptr};
    ScannedToken scanned;
    ScanStatus status = scan_literal(reader, final, error, token_offset, literal, kind, scanned);
    if (status == ScanStatus::NeedMore) {
        partial_.clear();
        if (!partial_.append(data + start, len - start)) {
            set_parse_error(error, "out of memory", token_offset);
            offset = len;
            return LexStatus::Error;
        }
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

LexStatus Lexer::continue_partial(const char *data, std::size_t len, std::size_t &offset, bool final, Token &out,
                                  ParseError &error) noexcept {
    token_storage_.clear();
    TokenReader reader{&partial_, data + offset, len - offset, &token_storage_};
    ScannedToken scanned;
    ScanStatus status = ScanStatus::Error;
    const unsigned char first = static_cast<unsigned char>(partial_.data[0]);

    if (first == '"') {
        status = scan_string(reader, final, error, partial_offset_, scanned);
    } else if (first == 't') {
        status = scan_literal(reader, final, error, partial_offset_, "true", TokenKind::Bool, scanned);
    } else if (first == 'f') {
        status = scan_literal(reader, final, error, partial_offset_, "false", TokenKind::Bool, scanned);
    } else if (first == 'n') {
        status = scan_literal(reader, final, error, partial_offset_, "null", TokenKind::Null, scanned);
    } else {
        status = scan_number(reader, final, error, partial_offset_, scanned);
    }

    offset += reader.input_consumed();
    if (status == ScanStatus::NeedMore) {
        Buffer tmp = partial_;
        partial_ = token_storage_;
        token_storage_ = tmp;
        token_storage_.clear();
        return LexStatus::NeedMore;
    }
    if (status == ScanStatus::Error) {
        return LexStatus::Error;
    }

    out.kind = scanned.kind;
    out.data = token_storage_.data + scanned.data_offset;
    out.len = scanned.data_len;
    out.offset = partial_offset_;
    partial_.clear();
    partial_offset_ = 0;
    return LexStatus::Ok;
}

LexStatus Lexer::next(const char *data, std::size_t len, std::size_t &offset, bool final, Token &out,
                      ParseError &error) noexcept {
    out = {};
    if (partial_.size > 0) {
        return continue_partial(data, len, offset, final, out, error);
    }

    token_storage_.clear();
    while (offset < len && is_ws(data[offset])) {
        offset += 1;
    }
    if (offset >= len) {
        out.kind = TokenKind::Eof;
        out.offset = stream_offset_ + offset;
        return final ? LexStatus::Ok : LexStatus::NeedMore;
    }

    const std::size_t token_offset = stream_offset_ + offset;
    const unsigned char ch = static_cast<unsigned char>(data[offset]);
    switch (ch) {
        case '{':
            out.kind = TokenKind::ObjectOpen;
            out.data = data + offset;
            out.len = 1;
            out.offset = token_offset;
            offset += 1;
            return LexStatus::Ok;
        case '}':
            out.kind = TokenKind::ObjectClose;
            out.data = data + offset;
            out.len = 1;
            out.offset = token_offset;
            offset += 1;
            return LexStatus::Ok;
        case '[':
            out.kind = TokenKind::ArrayOpen;
            out.data = data + offset;
            out.len = 1;
            out.offset = token_offset;
            offset += 1;
            return LexStatus::Ok;
        case ']':
            out.kind = TokenKind::ArrayClose;
            out.data = data + offset;
            out.len = 1;
            out.offset = token_offset;
            offset += 1;
            return LexStatus::Ok;
        case ':':
            out.kind = TokenKind::Colon;
            out.data = data + offset;
            out.len = 1;
            out.offset = token_offset;
            offset += 1;
            return LexStatus::Ok;
        case ',':
            out.kind = TokenKind::Comma;
            out.data = data + offset;
            out.len = 1;
            out.offset = token_offset;
            offset += 1;
            return LexStatus::Ok;
        case '"':
            return scan_current(data, len, offset, final, token_offset, out, error, scan_string);
        case 't':
            return scan_current_literal(data, len, offset, final, token_offset, "true", TokenKind::Bool, out, error);
        case 'f':
            return scan_current_literal(data, len, offset, final, token_offset, "false", TokenKind::Bool, out, error);
        case 'n':
            return scan_current_literal(data, len, offset, final, token_offset, "null", TokenKind::Null, out, error);
        default:
            if (ch == '-' || is_digit(ch)) {
                return scan_current(data, len, offset, final, token_offset, out, error, scan_number);
            }
            set_parse_error(error, "invalid token", token_offset);
            return LexStatus::Error;
    }
}

bool decode_string(TokenKind kind, const char *data, std::size_t len, Buffer &scratch, const char *&out,
                   std::size_t &out_len, ParseError &error, std::size_t offset) noexcept {
    if (kind == TokenKind::String) {
        out = data;
        out_len = len;
        return true;
    }

    scratch.clear();
    std::size_t pos = 0;
    while (pos < len) {
        const unsigned char ch = static_cast<unsigned char>(data[pos]);
        if (ch != '\\') {
            if (!scratch.append(static_cast<char>(ch))) {
                return set_parse_error(error, "out of memory", offset + pos);
            }
            pos += 1;
            continue;
        }

        pos += 1;
        if (pos >= len) {
            return set_parse_error(error, "unterminated escape sequence", offset + pos);
        }

        const char esc = data[pos++];
        switch (esc) {
            case '"':
            case '\\':
            case '/':
                if (!scratch.append(esc)) {
                    return set_parse_error(error, "out of memory", offset + pos);
                }
                break;
            case 'b':
                if (!scratch.append('\b')) {
                    return set_parse_error(error, "out of memory", offset + pos);
                }
                break;
            case 'f':
                if (!scratch.append('\f')) {
                    return set_parse_error(error, "out of memory", offset + pos);
                }
                break;
            case 'n':
                if (!scratch.append('\n')) {
                    return set_parse_error(error, "out of memory", offset + pos);
                }
                break;
            case 'r':
                if (!scratch.append('\r')) {
                    return set_parse_error(error, "out of memory", offset + pos);
                }
                break;
            case 't':
                if (!scratch.append('\t')) {
                    return set_parse_error(error, "out of memory", offset + pos);
                }
                break;
            case 'u': {
                if (pos + 4 > len) {
                    return set_parse_error(error, "invalid unicode escape", offset + pos);
                }
                std::uint32_t code = 0;
                for (int idx = 0; idx < 4; ++idx) {
                    int digit = hex_value(data[pos + idx]);
                    if (digit < 0) {
                        return set_parse_error(error, "invalid unicode escape",
                                               offset + pos + static_cast<std::size_t>(idx));
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
                            return set_parse_error(error, "invalid unicode escape",
                                                   offset + pos + static_cast<std::size_t>(idx));
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
                if (!append_utf8(scratch, code, error, offset + pos)) {
                    return false;
                }
                break;
            }
            default:
                return set_parse_error(error, "invalid escape sequence", offset + pos - 1);
        }
    }

    out = scratch.data;
    out_len = scratch.size;
    return true;
}

} // namespace fiber::json::detail
