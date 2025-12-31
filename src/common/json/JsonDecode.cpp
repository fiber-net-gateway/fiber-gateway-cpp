//
// Created by dear on 2025/12/30.
//

#include "JsonDecode.h"

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fiber::json {
namespace {

constexpr size_t kInitialContainerCapacity = 4;

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

bool set_parse_error(ParseError &error, const char *message, std::size_t offset);

void append_code_unit(DecodedString &out, char16_t unit) {
    if (out.is_byte && unit <= 0xFF) {
        out.bytes.push_back(static_cast<std::uint8_t>(unit));
        return;
    }
    if (out.is_byte) {
        out.is_byte = false;
        out.u16.reserve(out.bytes.size() + 1);
        for (std::uint8_t byte : out.bytes) {
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
    char16_t high = static_cast<char16_t>(0xD800 + (value >> 10));
    char16_t low = static_cast<char16_t>(0xDC00 + (value & 0x3FF));
    append_code_unit(out, high);
    append_code_unit(out, low);
}

enum class Utf8DecodeResult {
    Ok,
    NeedMore,
    Error,
};

Utf8DecodeResult decode_utf8_codepoint(const char *data, std::size_t len, std::size_t &pos, bool final,
                                       std::uint32_t &codepoint, ParseError &error, std::size_t offset_base) {
    if (pos >= len) {
        if (!final) {
            return Utf8DecodeResult::NeedMore;
        }
        set_parse_error(error, "invalid utf-8 sequence", offset_base + pos);
        return Utf8DecodeResult::Error;
    }
    unsigned char ch = static_cast<unsigned char>(data[pos]);
    if (ch < 0x80) {
        codepoint = ch;
        pos += 1;
        return Utf8DecodeResult::Ok;
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
        return Utf8DecodeResult::Error;
    }
    if (pos + static_cast<std::size_t>(needed) >= len) {
        if (!final) {
            return Utf8DecodeResult::NeedMore;
        }
        set_parse_error(error, "invalid utf-8 sequence", offset_base + pos);
        return Utf8DecodeResult::Error;
    }
    for (int idx = 1; idx <= needed; ++idx) {
        unsigned char next = static_cast<unsigned char>(data[pos + idx]);
        if ((next & 0xC0) != 0x80) {
            set_parse_error(error, "invalid utf-8 sequence", offset_base + pos + idx);
            return Utf8DecodeResult::Error;
        }
        code = (code << 6) | (next & 0x3F);
    }
    if (code < min_value || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        set_parse_error(error, "invalid utf-8 sequence", offset_base + pos);
        return Utf8DecodeResult::Error;
    }
    codepoint = code;
    pos += static_cast<std::size_t>(needed) + 1;
    return Utf8DecodeResult::Ok;
}

GcString *make_gc_string(GcHeap &heap, const DecodedString &decoded) {
    if (decoded.is_byte) {
        return gc_new_string_bytes(&heap, decoded.bytes.data(), decoded.bytes.size());
    }
    return gc_new_string_utf16(&heap, decoded.u16.data(), decoded.u16.size());
}

bool ensure_array_capacity(GcHeap &heap, GcArray *arr, std::size_t needed) {
    if (needed <= arr->capacity) {
        return true;
    }
    std::size_t new_capacity = arr->capacity ? arr->capacity * 2 : kInitialContainerCapacity;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    auto *new_elems = static_cast<JsValue *>(heap.alloc.alloc(sizeof(JsValue) * new_capacity));
    if (!new_elems) {
        return false;
    }
    for (std::size_t i = 0; i < new_capacity; ++i) {
        std::construct_at(&new_elems[i]);
    }
    for (std::size_t i = 0; i < arr->size; ++i) {
        new_elems[i] = std::move(arr->elems[i]);
    }
    if (arr->elems) {
        for (std::size_t i = 0; i < arr->capacity; ++i) {
            std::destroy_at(&arr->elems[i]);
        }
        heap.alloc.free(arr->elems);
    }
    arr->elems = new_elems;
    arr->capacity = new_capacity;
    return true;
}

bool ensure_object_capacity(GcHeap &heap, GcObject *obj, std::size_t needed) {
    if (needed <= obj->capacity) {
        return true;
    }
    std::size_t new_capacity = obj->capacity ? obj->capacity * 2 : kInitialContainerCapacity;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    auto *new_entries = static_cast<GcObjectEntry *>(heap.alloc.alloc(sizeof(GcObjectEntry) * new_capacity));
    if (!new_entries) {
        return false;
    }
    for (std::size_t i = 0; i < new_capacity; ++i) {
        new_entries[i].key = nullptr;
        std::construct_at(&new_entries[i].value);
    }
    for (std::size_t i = 0; i < obj->size; ++i) {
        new_entries[i].key = obj->entries[i].key;
        new_entries[i].value = std::move(obj->entries[i].value);
    }
    if (obj->entries) {
        for (std::size_t i = 0; i < obj->capacity; ++i) {
            std::destroy_at(&obj->entries[i].value);
        }
        heap.alloc.free(obj->entries);
    }
    obj->entries = new_entries;
    obj->capacity = new_capacity;
    return true;
}

bool set_parse_error(ParseError &error, const char *message, std::size_t offset) {
    if (error.message.empty()) {
        error.message = message;
        error.offset = offset;
    }
    return false;
}

bool is_ws(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

enum class TokenType {
    End,
    String,
    Number,
    True,
    False,
    Null,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    Colon,
    Comma,
};

enum class LexResult {
    Ok,
    NeedMore,
    Error,
};

struct LexToken {
    TokenType type = TokenType::End;
    DecodedString text;
    JsValue value;
    std::size_t offset = 0;
    std::size_t end = 0;
};

bool is_delimiter(char ch) {
    return is_ws(ch) || ch == ',' || ch == ']' || ch == '}' || ch == ':';
}

LexResult lex_string(const std::string &buffer, std::size_t start, bool final, LexToken &out, ParseError &error,
                     std::size_t total_offset) {
    std::size_t i = start + 1;
    DecodedString decoded;
    while (i < buffer.size()) {
        unsigned char ch = static_cast<unsigned char>(buffer[i]);
        if (ch == '\"') {
            out.type = TokenType::String;
            out.text = std::move(decoded);
            out.offset = total_offset + start;
            out.end = i + 1;
            return LexResult::Ok;
        }
        if (ch == '\\') {
            if (i + 1 >= buffer.size()) {
                if (!final) {
                    return LexResult::NeedMore;
                }
                set_parse_error(error, "unterminated escape sequence", total_offset + i);
                return LexResult::Error;
            }
            char esc = buffer[i + 1];
            i += 2;
            switch (esc) {
                case '\"':
                    append_code_unit(decoded, '\"');
                    break;
                case '\\':
                    append_code_unit(decoded, '\\');
                    break;
                case '/':
                    append_code_unit(decoded, '/');
                    break;
                case 'b':
                    append_code_unit(decoded, '\b');
                    break;
                case 'f':
                    append_code_unit(decoded, '\f');
                    break;
                case 'n':
                    append_code_unit(decoded, '\n');
                    break;
                case 'r':
                    append_code_unit(decoded, '\r');
                    break;
                case 't':
                    append_code_unit(decoded, '\t');
                    break;
                case 'u': {
                    if (i + 4 > buffer.size()) {
                        if (!final) {
                            return LexResult::NeedMore;
                        }
                        set_parse_error(error, "invalid unicode escape", total_offset + i);
                        return LexResult::Error;
                    }
                    uint32_t code = 0;
                    for (int idx = 0; idx < 4; ++idx) {
                        int digit = hex_value(buffer[i + idx]);
                        if (digit < 0) {
                            set_parse_error(error, "invalid unicode escape", total_offset + i + idx);
                            return LexResult::Error;
                        }
                        code = (code << 4) | static_cast<uint32_t>(digit);
                    }
                    i += 4;
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (i + 5 >= buffer.size()) {
                            if (!final) {
                                return LexResult::NeedMore;
                            }
                            set_parse_error(error, "invalid unicode surrogate pair", total_offset + i);
                            return LexResult::Error;
                        }
                        if (buffer[i] != '\\' || buffer[i + 1] != 'u') {
                            set_parse_error(error, "invalid unicode surrogate pair", total_offset + i);
                            return LexResult::Error;
                        }
                        i += 2;
                        uint32_t low = 0;
                        for (int idx = 0; idx < 4; ++idx) {
                            int digit = hex_value(buffer[i + idx]);
                            if (digit < 0) {
                                set_parse_error(error, "invalid unicode escape", total_offset + i + idx);
                                return LexResult::Error;
                            }
                            low = (low << 4) | static_cast<uint32_t>(digit);
                        }
                        i += 4;
                        if (low < 0xDC00 || low > 0xDFFF) {
                            set_parse_error(error, "invalid unicode surrogate pair", total_offset + i);
                            return LexResult::Error;
                        }
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        set_parse_error(error, "invalid unicode surrogate pair", total_offset + i);
                        return LexResult::Error;
                    }
                    if (code > 0x10FFFF) {
                        set_parse_error(error, "invalid unicode codepoint", total_offset + i);
                        return LexResult::Error;
                    }
                    append_codepoint(decoded, code);
                    break;
                }
                default:
                    set_parse_error(error, "invalid escape sequence", total_offset + i - 2);
                    return LexResult::Error;
            }
            continue;
        }
        if (ch < 0x20) {
            set_parse_error(error, "invalid control character in string", total_offset + i);
            return LexResult::Error;
        }
        std::size_t cursor = i;
        std::uint32_t codepoint = 0;
        Utf8DecodeResult result = decode_utf8_codepoint(buffer.data(), buffer.size(), cursor, final, codepoint,
                                                        error, total_offset);
        if (result == Utf8DecodeResult::NeedMore) {
            return LexResult::NeedMore;
        }
        if (result == Utf8DecodeResult::Error) {
            return LexResult::Error;
        }
        if (codepoint < 0x20) {
            set_parse_error(error, "invalid control character in string", total_offset + i);
            return LexResult::Error;
        }
        append_codepoint(decoded, codepoint);
        i = cursor;
        continue;
    }
    if (!final) {
        return LexResult::NeedMore;
    }
    set_parse_error(error, "unterminated string", total_offset + buffer.size());
    return LexResult::Error;
}

LexResult lex_literal(const std::string &buffer, std::size_t start, bool final,
                      const char *literal, TokenType type, LexToken &out, ParseError &error,
                      std::size_t total_offset) {
    std::size_t len = std::strlen(literal);
    if (start + len > buffer.size()) {
        if (!final) {
            return LexResult::NeedMore;
        }
        set_parse_error(error, "invalid literal", total_offset + start);
        return LexResult::Error;
    }
    for (std::size_t i = 0; i < len; ++i) {
        if (buffer[start + i] != literal[i]) {
            set_parse_error(error, "invalid literal", total_offset + start + i);
            return LexResult::Error;
        }
    }
    out.type = type;
    out.offset = total_offset + start;
    out.end = start + len;
    return LexResult::Ok;
}

LexResult lex_number(const std::string &buffer, std::size_t start, bool final, LexToken &out, ParseError &error,
                     std::size_t total_offset) {
    std::size_t i = start;
    bool is_float = false;
    if (buffer[i] == '-') {
        i += 1;
        if (i >= buffer.size()) {
            return final ? (set_parse_error(error, "invalid number", total_offset + start), LexResult::Error)
                         : LexResult::NeedMore;
        }
    }
    if (buffer[i] == '0') {
        i += 1;
        if (i < buffer.size() && buffer[i] >= '0' && buffer[i] <= '9') {
            set_parse_error(error, "leading zero in number", total_offset + start);
            return LexResult::Error;
        }
    } else if (buffer[i] >= '1' && buffer[i] <= '9') {
        while (i < buffer.size() && buffer[i] >= '0' && buffer[i] <= '9') {
            i += 1;
        }
    } else {
        set_parse_error(error, "invalid number", total_offset + start);
        return LexResult::Error;
    }
    if (i < buffer.size() && buffer[i] == '.') {
        is_float = true;
        i += 1;
        if (i >= buffer.size()) {
            return final ? (set_parse_error(error, "invalid number", total_offset + start), LexResult::Error)
                         : LexResult::NeedMore;
        }
        if (buffer[i] < '0' || buffer[i] > '9') {
            set_parse_error(error, "invalid number", total_offset + start);
            return LexResult::Error;
        }
        while (i < buffer.size() && buffer[i] >= '0' && buffer[i] <= '9') {
            i += 1;
        }
    }
    if (i < buffer.size() && (buffer[i] == 'e' || buffer[i] == 'E')) {
        is_float = true;
        i += 1;
        if (i >= buffer.size()) {
            return final ? (set_parse_error(error, "invalid number", total_offset + start), LexResult::Error)
                         : LexResult::NeedMore;
        }
        if (buffer[i] == '+' || buffer[i] == '-') {
            i += 1;
            if (i >= buffer.size()) {
                return final ? (set_parse_error(error, "invalid number", total_offset + start), LexResult::Error)
                             : LexResult::NeedMore;
            }
        }
        if (buffer[i] < '0' || buffer[i] > '9') {
            set_parse_error(error, "invalid number", total_offset + start);
            return LexResult::Error;
        }
        while (i < buffer.size() && buffer[i] >= '0' && buffer[i] <= '9') {
            i += 1;
        }
    }
    if (i == buffer.size()) {
        if (!final) {
            return LexResult::NeedMore;
        }
    } else if (!is_delimiter(buffer[i])) {
        set_parse_error(error, "invalid number", total_offset + start);
        return LexResult::Error;
    }
    const char *num_start = buffer.data() + start;
    const char *num_end = buffer.data() + i;
    if (is_float) {
        std::string number(num_start, num_end);
        errno = 0;
        char *end_ptr = nullptr;
        double value = std::strtod(number.c_str(), &end_ptr);
        if (errno == ERANGE) {
            set_parse_error(error, "floating point overflow", total_offset + start);
            return LexResult::Error;
        }
        if (end_ptr != number.c_str() + number.size()) {
            set_parse_error(error, "invalid number", total_offset + start);
            return LexResult::Error;
        }
        out.type = TokenType::Number;
        out.value = JsValue::make_float(value);
    } else {
        int64_t value = 0;
        auto result = std::from_chars(num_start, num_end, value);
        if (result.ec == std::errc::result_out_of_range) {
            set_parse_error(error, "integer overflow", total_offset + start);
            return LexResult::Error;
        }
        if (result.ec != std::errc()) {
            set_parse_error(error, "invalid number", total_offset + start);
            return LexResult::Error;
        }
        out.type = TokenType::Number;
        out.value = JsValue::make_integer(value);
    }
    out.offset = total_offset + start;
    out.end = i;
    return LexResult::Ok;
}

LexResult lex_token(const std::string &buffer, std::size_t &pos, bool final, LexToken &out, ParseError &error,
                    std::size_t total_offset) {
    std::size_t i = pos;
    while (i < buffer.size() && is_ws(buffer[i])) {
        i += 1;
    }
    pos = i;
    if (i >= buffer.size()) {
        if (final) {
            out.type = TokenType::End;
            out.offset = total_offset + i;
            return LexResult::Ok;
        }
        return LexResult::NeedMore;
    }
    char ch = buffer[i];
    switch (ch) {
        case '{':
            out.type = TokenType::LeftBrace;
            out.offset = total_offset + i;
            out.end = i + 1;
            pos = out.end;
            return LexResult::Ok;
        case '}':
            out.type = TokenType::RightBrace;
            out.offset = total_offset + i;
            out.end = i + 1;
            pos = out.end;
            return LexResult::Ok;
        case '[':
            out.type = TokenType::LeftBracket;
            out.offset = total_offset + i;
            out.end = i + 1;
            pos = out.end;
            return LexResult::Ok;
        case ']':
            out.type = TokenType::RightBracket;
            out.offset = total_offset + i;
            out.end = i + 1;
            pos = out.end;
            return LexResult::Ok;
        case ':':
            out.type = TokenType::Colon;
            out.offset = total_offset + i;
            out.end = i + 1;
            pos = out.end;
            return LexResult::Ok;
        case ',':
            out.type = TokenType::Comma;
            out.offset = total_offset + i;
            out.end = i + 1;
            pos = out.end;
            return LexResult::Ok;
        case '\"': {
            LexResult result = lex_string(buffer, i, final, out, error, total_offset);
            if (result == LexResult::Ok) {
                pos = out.end;
            }
            return result;
        }
        case 't':
        {
            LexResult result = lex_literal(buffer, i, final, "true", TokenType::True, out, error, total_offset);
            if (result == LexResult::Ok) {
                pos = out.end;
            }
            return result;
        }
        case 'f':
        {
            LexResult result = lex_literal(buffer, i, final, "false", TokenType::False, out, error, total_offset);
            if (result == LexResult::Ok) {
                pos = out.end;
            }
            return result;
        }
        case 'n':
        {
            LexResult result = lex_literal(buffer, i, final, "null", TokenType::Null, out, error, total_offset);
            if (result == LexResult::Ok) {
                pos = out.end;
            }
            return result;
        }
        default:
            if (ch == '-' || (ch >= '0' && ch <= '9')) {
                LexResult result = lex_number(buffer, i, final, out, error, total_offset);
                if (result == LexResult::Ok) {
                    pos = out.end;
                }
                return result;
            }
            set_parse_error(error, "invalid token", total_offset + i);
            return LexResult::Error;
    }
}

class ParserImpl {
public:
    ParserImpl(GcHeap &heap, ParseError &error, const char *data, std::size_t len)
        : heap_(heap), error_(error), data_(data), len_(len) {}

    bool parse(JsValue &out) {
        skip_ws();
        if (!parse_value(out)) {
            return false;
        }
        skip_ws();
        if (pos_ != len_) {
            return set_error("trailing characters after JSON value", pos_);
        }
        return true;
    }

private:
    bool parse_value(JsValue &out) {
        skip_ws();
        if (pos_ >= len_) {
            return set_error("unexpected end of input", pos_);
        }
        char ch = data_[pos_];
        if (ch == '\"') {
            DecodedString value;
            if (!parse_string(value)) {
                return false;
            }
            GcString *str = make_gc_string(heap_, value);
            if (!str) {
                return set_error("out of memory", pos_);
            }
            out.type_ = JsNodeType::HeapString;
            out.gc = &str->hdr;
            return true;
        }
        if (ch == '{') {
            return parse_object(out);
        }
        if (ch == '[') {
            return parse_array(out);
        }
        if (ch == 't') {
            return parse_literal("true", out, JsValue::make_boolean(true));
        }
        if (ch == 'f') {
            return parse_literal("false", out, JsValue::make_boolean(false));
        }
        if (ch == 'n') {
            return parse_literal("null", out, JsValue::make_null());
        }
        if (ch == '-' || (ch >= '0' && ch <= '9')) {
            return parse_number(out);
        }
        return set_error("invalid token", pos_);
    }

    bool parse_object(JsValue &out) {
        if (data_[pos_] != '{') {
            return set_error("expected '{'", pos_);
        }
        pos_ += 1;
        skip_ws();
        GcObject *obj = gc_new_object(&heap_, kInitialContainerCapacity);
        if (!obj) {
            return set_error("out of memory", pos_);
        }
        out.type_ = JsNodeType::Object;
        out.gc = &obj->hdr;
        if (pos_ < len_ && data_[pos_] == '}') {
            pos_ += 1;
            return true;
        }
        while (true) {
            skip_ws();
            if (pos_ >= len_) {
                return set_error("unexpected end of input", pos_);
            }
            if (data_[pos_] != '\"') {
                return set_error("object key must be a string", pos_);
            }
            DecodedString key;
            if (!parse_string(key)) {
                return false;
            }
            skip_ws();
            if (pos_ >= len_ || data_[pos_] != ':') {
                return set_error("expected ':' after object key", pos_);
            }
            pos_ += 1;
            JsValue value;
            if (!parse_value(value)) {
                return false;
            }
            if (!ensure_object_capacity(heap_, obj, obj->size + 1)) {
                return set_error("out of memory", pos_);
            }
            GcString *key_str = make_gc_string(heap_, key);
            if (!key_str) {
                return set_error("out of memory", pos_);
            }
            obj->entries[obj->size].key = key_str;
            obj->entries[obj->size].value = std::move(value);
            obj->size += 1;
            skip_ws();
            if (pos_ >= len_) {
                return set_error("unexpected end of input", pos_);
            }
            if (data_[pos_] == ',') {
                pos_ += 1;
                continue;
            }
            if (data_[pos_] == '}') {
                pos_ += 1;
                return true;
            }
            return set_error("expected ',' or '}' after object value", pos_);
        }
    }

    bool parse_array(JsValue &out) {
        if (data_[pos_] != '[') {
            return set_error("expected '['", pos_);
        }
        pos_ += 1;
        skip_ws();
        GcArray *arr = gc_new_array(&heap_, kInitialContainerCapacity);
        if (!arr) {
            return set_error("out of memory", pos_);
        }
        out.type_ = JsNodeType::Array;
        out.gc = &arr->hdr;
        if (pos_ < len_ && data_[pos_] == ']') {
            pos_ += 1;
            return true;
        }
        while (true) {
            JsValue value;
            if (!parse_value(value)) {
                return false;
            }
            if (!ensure_array_capacity(heap_, arr, arr->size + 1)) {
                return set_error("out of memory", pos_);
            }
            arr->elems[arr->size] = std::move(value);
            arr->size += 1;
            skip_ws();
            if (pos_ >= len_) {
                return set_error("unexpected end of input", pos_);
            }
            if (data_[pos_] == ',') {
                pos_ += 1;
                continue;
            }
            if (data_[pos_] == ']') {
                pos_ += 1;
                return true;
            }
            return set_error("expected ',' or ']' after array value", pos_);
        }
    }

    bool parse_string(DecodedString &out) {
        if (data_[pos_] != '\"') {
            return set_error("expected string", pos_);
        }
        pos_ += 1;
        out.clear();
        while (pos_ < len_) {
            unsigned char ch = static_cast<unsigned char>(data_[pos_]);
            if (ch == '\"') {
                pos_ += 1;
                return true;
            }
            if (ch == '\\') {
                pos_ += 1;
                if (pos_ >= len_) {
                    return set_error("unterminated escape sequence", pos_);
                }
                char esc = data_[pos_];
                pos_ += 1;
                switch (esc) {
                    case '\"':
                        append_code_unit(out, '\"');
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
                        uint32_t code = 0;
                        if (!parse_hex(code)) {
                            return false;
                        }
                        if (code >= 0xD800 && code <= 0xDBFF) {
                            if (pos_ + 1 >= len_ || data_[pos_] != '\\' || data_[pos_ + 1] != 'u') {
                                return set_error("invalid unicode surrogate pair", pos_);
                            }
                            pos_ += 2;
                            uint32_t low = 0;
                            if (!parse_hex(low)) {
                                return false;
                            }
                            if (low < 0xDC00 || low > 0xDFFF) {
                                return set_error("invalid unicode surrogate pair", pos_);
                            }
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        } else if (code >= 0xDC00 && code <= 0xDFFF) {
                            return set_error("invalid unicode surrogate pair", pos_);
                        }
                        if (code > 0x10FFFF) {
                            return set_error("invalid unicode codepoint", pos_);
                        }
                        append_codepoint(out, code);
                        break;
                    }
                    default:
                        return set_error("invalid escape sequence", pos_ - 1);
                }
                continue;
            }
            if (ch < 0x20) {
                return set_error("invalid control character in string", pos_);
            }
            std::size_t cursor = pos_;
            std::uint32_t codepoint = 0;
            Utf8DecodeResult result = decode_utf8_codepoint(data_, len_, cursor, true, codepoint, error_, 0);
            if (result == Utf8DecodeResult::Error) {
                return false;
            }
            if (codepoint < 0x20) {
                return set_error("invalid control character in string", pos_);
            }
            append_codepoint(out, codepoint);
            pos_ = cursor;
        }
        return set_error("unterminated string", pos_);
    }

    bool parse_number(JsValue &out) {
        std::size_t start = pos_;
        bool is_float = false;
        if (data_[pos_] == '-') {
            pos_ += 1;
            if (pos_ >= len_) {
                return set_error("invalid number", start);
            }
        }
        if (data_[pos_] == '0') {
            pos_ += 1;
            if (pos_ < len_ && data_[pos_] >= '0' && data_[pos_] <= '9') {
                return set_error("leading zero in number", start);
            }
        } else if (data_[pos_] >= '1' && data_[pos_] <= '9') {
            while (pos_ < len_ && data_[pos_] >= '0' && data_[pos_] <= '9') {
                pos_ += 1;
            }
        } else {
            return set_error("invalid number", start);
        }
        if (pos_ < len_ && data_[pos_] == '.') {
            is_float = true;
            pos_ += 1;
            if (pos_ >= len_ || data_[pos_] < '0' || data_[pos_] > '9') {
                return set_error("invalid number", start);
            }
            while (pos_ < len_ && data_[pos_] >= '0' && data_[pos_] <= '9') {
                pos_ += 1;
            }
        }
        if (pos_ < len_ && (data_[pos_] == 'e' || data_[pos_] == 'E')) {
            is_float = true;
            pos_ += 1;
            if (pos_ < len_ && (data_[pos_] == '+' || data_[pos_] == '-')) {
                pos_ += 1;
            }
            if (pos_ >= len_ || data_[pos_] < '0' || data_[pos_] > '9') {
                return set_error("invalid number", start);
            }
            while (pos_ < len_ && data_[pos_] >= '0' && data_[pos_] <= '9') {
                pos_ += 1;
            }
        }
        const char *num_start = data_ + start;
        const char *num_end = data_ + pos_;
        if (is_float) {
            std::string number(num_start, num_end);
            errno = 0;
            char *end_ptr = nullptr;
            double value = std::strtod(number.c_str(), &end_ptr);
            if (errno == ERANGE) {
                return set_error("floating point overflow", start);
            }
            if (end_ptr != number.c_str() + number.size()) {
                return set_error("invalid number", start);
            }
            out = JsValue::make_float(value);
            return true;
        }
        int64_t value = 0;
        auto result = std::from_chars(num_start, num_end, value);
        if (result.ec == std::errc::result_out_of_range) {
            return set_error("integer overflow", start);
        }
        if (result.ec != std::errc()) {
            return set_error("invalid number", start);
        }
        out = JsValue::make_integer(value);
        return true;
    }

    bool parse_hex(uint32_t &out) {
        if (pos_ + 4 > len_) {
            return set_error("invalid unicode escape", pos_);
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            int digit = hex_value(data_[pos_ + i]);
            if (digit < 0) {
                return set_error("invalid unicode escape", pos_ + i);
            }
            value = (value << 4) | static_cast<uint32_t>(digit);
        }
        pos_ += 4;
        out = value;
        return true;
    }

    bool parse_literal(const char *literal, JsValue &out, const JsValue &value) {
        const char *cursor = data_ + pos_;
        for (const char *p = literal; *p != '\0'; ++p) {
            if (cursor >= data_ + len_ || *cursor != *p) {
                return set_error("invalid literal", pos_);
            }
            cursor += 1;
        }
        pos_ += static_cast<std::size_t>(cursor - (data_ + pos_));
        out = value;
        return true;
    }

    void skip_ws() {
        while (pos_ < len_) {
            char ch = data_[pos_];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                pos_ += 1;
                continue;
            }
            break;
        }
    }

    bool set_error(const char *message, std::size_t offset) {
        if (error_.message.empty()) {
            error_.message = message;
            error_.offset = offset;
        }
        return false;
    }

    GcHeap &heap_;
    ParseError &error_;
    const char *data_ = nullptr;
    std::size_t len_ = 0;
    std::size_t pos_ = 0;
};

} // namespace

Parser::Parser(GcHeap &heap)
    : heap_(heap) {}

bool Parser::parse(const char *data, std::size_t len, JsValue &out) {
    error_ = {};
    if (!data && len > 0) {
        error_.message = "input is null";
        error_.offset = 0;
        return false;
    }
    ParserImpl impl(heap_, error_, data ? data : "", len);
    return impl.parse(out);
}

bool Parser::parse(const std::string &data, JsValue &out) {
    return parse(data.data(), data.size(), out);
}

const ParseError &Parser::error() const {
    return error_;
}

StreamParser::StreamParser(GcHeap &heap)
    : heap_(heap) {
    reset();
}

void StreamParser::reset() {
    clear_error();
    root_ = JsValue();
    has_result_ = false;
    complete_ = false;
    buffer_.clear();
    pos_ = 0;
    total_offset_ = 0;
    state_stack_.clear();
    state_stack_.push_back(ParseState::Start);
    containers_.clear();
}

StreamParser::Status StreamParser::parse(const char *data, std::size_t len) {
    if (!data && len > 0) {
        (void)set_error("input is null", total_offset_ + pos_);
        return Status::Error;
    }
    if (complete_) {
        buffer_.append(data ? data : "", len);
        return parse_internal(false);
    }
    buffer_.append(data ? data : "", len);
    return parse_internal(false);
}

StreamParser::Status StreamParser::finish() {
    return parse_internal(true);
}

const ParseError &StreamParser::error() const {
    return error_;
}

const JsValue &StreamParser::root() const {
    return root_;
}

bool StreamParser::has_result() const {
    return has_result_;
}

StreamParser::Status StreamParser::parse_internal(bool final) {
    auto current_state = [&]() -> ParseState & {
        return state_stack_.back();
    };

    auto can_accept_value = [&]() -> bool {
        ParseState state = current_state();
        return state == ParseState::Start || state == ParseState::MapNeedVal ||
               state == ParseState::ArrayNeedVal || state == ParseState::ArrayStart;
    };

    auto value_complete = [&](std::size_t offset) -> bool {
        ParseState &state = current_state();
        switch (state) {
            case ParseState::Start:
                state = ParseState::ParseComplete;
                return true;
            case ParseState::MapNeedVal:
                state = ParseState::MapGotVal;
                return true;
            case ParseState::ArrayNeedVal:
            case ParseState::ArrayStart:
                state = ParseState::ArrayGotVal;
                return true;
            default:
                return set_error("unexpected value", offset);
        }
    };

    auto add_value = [&](JsValue &&value, std::size_t offset) -> bool {
        if (containers_.empty()) {
            if (has_result_) {
                return set_error("multiple top-level values", offset);
            }
            root_ = std::move(value);
            has_result_ = true;
            return true;
        }
        ContainerFrame &frame = containers_.back();
        if (frame.type == JsNodeType::Array) {
            if (!ensure_array_capacity(heap_, frame.array, frame.array->size + 1)) {
                return set_error("out of memory", offset);
            }
            frame.array->elems[frame.array->size] = std::move(value);
            frame.array->size += 1;
            return true;
        }
        if (frame.type == JsNodeType::Object) {
            if (!frame.has_key) {
                return set_error("object value missing key", offset);
            }
            if (!ensure_object_capacity(heap_, frame.object, frame.object->size + 1)) {
                return set_error("out of memory", offset);
            }
            GcString *key = make_gc_string(heap_, frame.key);
            if (!key) {
                return set_error("out of memory", offset);
            }
            frame.object->entries[frame.object->size].key = key;
            frame.object->entries[frame.object->size].value = std::move(value);
            frame.object->size += 1;
            frame.key.clear();
            frame.has_key = false;
            return true;
        }
        return set_error("invalid container state", offset);
    };

    auto begin_object = [&](std::size_t offset) -> bool {
        if (!can_accept_value()) {
            return set_error("unexpected '{'", offset);
        }
        GcObject *obj = gc_new_object(&heap_, kInitialContainerCapacity);
        if (!obj) {
            return set_error("out of memory", offset);
        }
        JsValue value;
        value.type_ = JsNodeType::Object;
        value.gc = &obj->hdr;
        if (!add_value(std::move(value), offset)) {
            return false;
        }
        ContainerFrame frame;
        frame.type = JsNodeType::Object;
        frame.object = obj;
        containers_.push_back(frame);
        state_stack_.push_back(ParseState::MapStart);
        return true;
    };

    auto begin_array = [&](std::size_t offset) -> bool {
        if (!can_accept_value()) {
            return set_error("unexpected '['", offset);
        }
        GcArray *arr = gc_new_array(&heap_, kInitialContainerCapacity);
        if (!arr) {
            return set_error("out of memory", offset);
        }
        JsValue value;
        value.type_ = JsNodeType::Array;
        value.gc = &arr->hdr;
        if (!add_value(std::move(value), offset)) {
            return false;
        }
        ContainerFrame frame;
        frame.type = JsNodeType::Array;
        frame.array = arr;
        containers_.push_back(frame);
        state_stack_.push_back(ParseState::ArrayStart);
        return true;
    };

    auto close_container = [&](JsNodeType type, std::size_t offset) -> bool {
        if (containers_.empty() || containers_.back().type != type) {
            return set_error("mismatched container close", offset);
        }
        containers_.pop_back();
        if (state_stack_.size() <= 1) {
            return set_error("invalid parser state", offset);
        }
        state_stack_.pop_back();
        return value_complete(offset);
    };

    auto value_from_token = [&](const LexToken &tok, JsValue &value) -> bool {
        switch (tok.type) {
            case TokenType::String: {
                GcString *str = make_gc_string(heap_, tok.text);
                if (!str) {
                    return set_error("out of memory", tok.offset);
                }
                value.type_ = JsNodeType::HeapString;
                value.gc = &str->hdr;
                return true;
            }
            case TokenType::Number:
                value = tok.value;
                return true;
            case TokenType::True:
                value = JsValue::make_boolean(true);
                return true;
            case TokenType::False:
                value = JsValue::make_boolean(false);
                return true;
            case TokenType::Null:
                value = JsValue::make_null();
                return true;
            default:
                return set_error("invalid value token", tok.offset);
        }
    };

    while (true) {
        if (current_state() == ParseState::ParseComplete) {
            while (pos_ < buffer_.size() && is_ws(buffer_[pos_])) {
                pos_ += 1;
            }
            if (pos_ == buffer_.size()) {
                compact_buffer();
                complete_ = true;
                return Status::Complete;
            }
            LexToken extra;
            LexResult extra_result = lex_token(buffer_, pos_, final, extra, error_, total_offset_);
            if (extra_result == LexResult::NeedMore) {
                compact_buffer();
                complete_ = true;
                return Status::Complete;
            }
            if (extra_result == LexResult::Error) {
                current_state() = ParseState::ParseError;
                return Status::Error;
            }
            (void)set_error("trailing garbage after JSON value", extra.offset);
            current_state() = ParseState::ParseError;
            return Status::Error;
        }

        LexToken tok;
        LexResult result = lex_token(buffer_, pos_, final, tok, error_, total_offset_);
        if (result == LexResult::NeedMore) {
            compact_buffer();
            return Status::NeedMore;
        }
        if (result == LexResult::Error) {
            current_state() = ParseState::ParseError;
            return Status::Error;
        }
        if (tok.type == TokenType::End) {
            if (final) {
                (void)set_error("premature EOF", total_offset_ + pos_);
                current_state() = ParseState::ParseError;
                return Status::Error;
            }
            compact_buffer();
            return Status::NeedMore;
        }

        ParseState &state = current_state();
        switch (state) {
            case ParseState::MapStart:
            case ParseState::MapNeedKey:
                if (tok.type == TokenType::RightBrace && state == ParseState::MapStart) {
                    if (!close_container(JsNodeType::Object, tok.offset)) {
                        current_state() = ParseState::ParseError;
                        return Status::Error;
                    }
                    break;
                }
                if (tok.type != TokenType::String) {
                    (void)set_error("object key must be a string", tok.offset);
                    current_state() = ParseState::ParseError;
                    return Status::Error;
                }
                if (containers_.empty() || containers_.back().type != JsNodeType::Object) {
                    (void)set_error("invalid object state", tok.offset);
                    current_state() = ParseState::ParseError;
                    return Status::Error;
                }
                containers_.back().key = tok.text;
                containers_.back().has_key = true;
                state = ParseState::MapSep;
                break;
            case ParseState::MapSep:
                if (tok.type != TokenType::Colon) {
                    (void)set_error("object key and value must be separated by ':'", tok.offset);
                    current_state() = ParseState::ParseError;
                    return Status::Error;
                }
                state = ParseState::MapNeedVal;
                break;
            case ParseState::MapGotVal:
                if (tok.type == TokenType::RightBrace) {
                    if (!close_container(JsNodeType::Object, tok.offset)) {
                        current_state() = ParseState::ParseError;
                        return Status::Error;
                    }
                    break;
                }
                if (tok.type == TokenType::Comma) {
                    state = ParseState::MapNeedKey;
                    break;
                }
                (void)set_error("after object value, expected ',' or '}'", tok.offset);
                current_state() = ParseState::ParseError;
                return Status::Error;
            case ParseState::ArrayStart:
                if (tok.type == TokenType::RightBracket) {
                    if (!close_container(JsNodeType::Array, tok.offset)) {
                        current_state() = ParseState::ParseError;
                        return Status::Error;
                    }
                    break;
                }
                [[fallthrough]];
            case ParseState::ArrayNeedVal:
            case ParseState::MapNeedVal:
            case ParseState::Start: {
                if (!can_accept_value()) {
                    (void)set_error("unexpected token", tok.offset);
                    current_state() = ParseState::ParseError;
                    return Status::Error;
                }
                if (tok.type == TokenType::LeftBrace) {
                    if (!begin_object(tok.offset)) {
                        current_state() = ParseState::ParseError;
                        return Status::Error;
                    }
                    break;
                }
                if (tok.type == TokenType::LeftBracket) {
                    if (!begin_array(tok.offset)) {
                        current_state() = ParseState::ParseError;
                        return Status::Error;
                    }
                    break;
                }
                JsValue value;
                if (!value_from_token(tok, value)) {
                    current_state() = ParseState::ParseError;
                    return Status::Error;
                }
                if (!add_value(std::move(value), tok.offset)) {
                    current_state() = ParseState::ParseError;
                    return Status::Error;
                }
                if (!value_complete(tok.offset)) {
                    current_state() = ParseState::ParseError;
                    return Status::Error;
                }
                break;
            }
            case ParseState::ArrayGotVal:
                if (tok.type == TokenType::RightBracket) {
                    if (!close_container(JsNodeType::Array, tok.offset)) {
                        current_state() = ParseState::ParseError;
                        return Status::Error;
                    }
                    break;
                }
                if (tok.type == TokenType::Comma) {
                    state = ParseState::ArrayNeedVal;
                    break;
                }
                (void)set_error("after array value, expected ',' or ']'", tok.offset);
                current_state() = ParseState::ParseError;
                return Status::Error;
            case ParseState::ParseComplete:
            case ParseState::ParseError:
                (void)set_error("invalid parser state", tok.offset);
                current_state() = ParseState::ParseError;
                return Status::Error;
        }
    }
}

void StreamParser::compact_buffer() {
    if (pos_ == 0) {
        return;
    }
    total_offset_ += pos_;
    buffer_.erase(0, pos_);
    pos_ = 0;
}

void StreamParser::clear_error() {
    error_ = {};
}

bool StreamParser::set_error(const char *message, std::size_t offset) {
    return set_parse_error(error_, message, offset);
}

} // namespace fiber::json
