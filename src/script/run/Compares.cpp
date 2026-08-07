#include <fiber/script/run/Compares.h>

#include <fiber/common/json/Utf.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/gc/Wtf8.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace fiber::script::run {

namespace {

bool is_string_type(fiber::script::JsNodeType type) noexcept { return type == fiber::script::JsNodeType::String; }

bool is_number_type(fiber::script::JsNodeType type) noexcept {
    return type == fiber::script::JsNodeType::Integer || type == fiber::script::JsNodeType::Float;
}

bool is_numeric_like(fiber::script::JsNodeType type) noexcept {
    return type == fiber::script::JsNodeType::Integer || type == fiber::script::JsNodeType::Float ||
           type == fiber::script::JsNodeType::Boolean || type == fiber::script::JsNodeType::Null;
}

const fiber::script::GcString *as_heap_string(const fiber::script::JsValue &value) noexcept {
    return fiber::script::js_value_type(value) == fiber::script::JsNodeType::String
                   ? fiber::script::js_value_heap_ptr<const fiber::script::GcString>(value)
                   : nullptr;
}

bool to_number(const fiber::script::JsValue &value, double &out) noexcept {
    switch (fiber::script::js_value_type(value)) {
        case fiber::script::JsNodeType::Integer:
            out = static_cast<double>(fiber::script::js_value_int64(value));
            return true;
        case fiber::script::JsNodeType::Float:
            out = fiber::script::js_value_double(value);
            return true;
        case fiber::script::JsNodeType::Boolean:
            out = fiber::script::js_value_bool(value) ? 1.0 : 0.0;
            return true;
        case fiber::script::JsNodeType::Null:
            out = 0.0;
            return true;
        default:
            return false;
    }
}

bool is_truthy(const fiber::script::JsValue &value) noexcept {
    switch (fiber::script::js_value_type(value)) {
        case fiber::script::JsNodeType::Undefined:
        case fiber::script::JsNodeType::Null:
            return false;
        case fiber::script::JsNodeType::Boolean:
            return fiber::script::js_value_bool(value);
        case fiber::script::JsNodeType::Integer:
            return fiber::script::js_value_int64(value) != 0;
        case fiber::script::JsNodeType::Float:
            return fiber::script::js_value_double(value) != 0.0 && !std::isnan(fiber::script::js_value_double(value));
        case fiber::script::JsNodeType::String:
            if (fiber::script::js_value_is_borrowed_string(value)) {
                return fiber::script::js_value_native_string(value).len > 0;
            }
            if (auto *str = as_heap_string(value)) {
                return str->utf16_len > 0;
            }
            return false;
        case fiber::script::JsNodeType::Binary:
        case fiber::script::JsNodeType::Array:
        case fiber::script::JsNodeType::Object:
        case fiber::script::JsNodeType::Interator:
        case fiber::script::JsNodeType::Exception:
            return true;
    }
    return false;
}

enum class StringKind : std::uint8_t {
    HeapWtf8,
    NativeUtf8,
};

bool ascii_is_space(char16_t ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

bool ascii_is_digit(char16_t ch) noexcept { return ch >= '0' && ch <= '9'; }

int ascii_hex_digit(char16_t ch) noexcept {
    if (ch >= '0' && ch <= '9') {
        return static_cast<int>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<int>(ch - 'a') + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<int>(ch - 'A') + 10;
    }
    return -1;
}

bool ascii_eq_ci(char16_t lhs, char rhs) noexcept {
    if (lhs >= 'A' && lhs <= 'Z') {
        lhs = static_cast<char16_t>(lhs + ('a' - 'A'));
    }
    if (rhs >= 'A' && rhs <= 'Z') {
        rhs = static_cast<char>(rhs + ('a' - 'A'));
    }
    return lhs == static_cast<unsigned char>(rhs);
}

void clamp_exp_add(int &exp, int delta) noexcept {
    constexpr int kExpCap = 100000;
    if (delta > 0) {
        exp = exp >= kExpCap ? kExpCap : exp + 1;
    } else if (delta < 0) {
        exp = exp <= -kExpCap ? -kExpCap : exp - 1;
    }
}

void clamp_exp_add_many(int &exp, int delta) noexcept {
    constexpr int kExpCap = 100000;
    if (delta > 0) {
        exp = exp > kExpCap - delta ? kExpCap : exp + delta;
    } else if (delta < 0) {
        exp = exp < -kExpCap - delta ? -kExpCap : exp + delta;
    }
}

struct StringCursor {
    StringKind kind = StringKind::NativeUtf8;
    const char *utf8 = nullptr;
    std::size_t len = 0;
    std::size_t pos = 0;
    bool has_pending = false;
    char16_t pending = 0;
    bool malformed = false;
    fiber::script::gc_detail::Wtf8Cursor wtf8;
};

bool init_heap_string_cursor(const fiber::script::GcString *str, StringCursor &out) noexcept {
    out = {};
    if (!str) {
        return false;
    }
    out.kind = StringKind::HeapWtf8;
    out.wtf8.data = fiber::script::gc_string_wtf8_data(str);
    out.wtf8.len = fiber::script::gc_string_byte_len(str);
    return true;
}

bool init_native_string_cursor(fiber::script::NativeStr native, StringCursor &out) noexcept {
    out = {};
    out.kind = StringKind::NativeUtf8;
    out.utf8 = native.data;
    out.len = native.len;
    return out.len == 0 || out.utf8;
}

bool init_string_cursor(const fiber::script::JsValue &value, StringCursor &out) noexcept {
    if (fiber::script::js_value_type(value) != fiber::script::JsNodeType::String) {
        return false;
    }
    if (!fiber::script::js_value_is_borrowed_string(value)) {
        return init_heap_string_cursor(as_heap_string(value), out);
    }
    return init_native_string_cursor(fiber::script::js_value_native_string(value), out);
}

bool cursor_next(StringCursor &cursor, char16_t &unit) noexcept {
    switch (cursor.kind) {
        case StringKind::HeapWtf8: {
            const bool has_unit = fiber::script::gc_detail::wtf8_next_utf16_unit(cursor.wtf8, unit);
            cursor.malformed = cursor.wtf8.malformed;
            return has_unit;
        }
        case StringKind::NativeUtf8: {
            if (cursor.has_pending) {
                unit = cursor.pending;
                cursor.has_pending = false;
                return true;
            }
            if (cursor.pos >= cursor.len) {
                return false;
            }
            std::uint32_t codepoint = 0;
            if (!fiber::json::utf8_next_codepoint(cursor.utf8, cursor.len, cursor.pos, codepoint)) {
                cursor.malformed = true;
                return false;
            }
            if (codepoint <= 0xFFFF) {
                unit = static_cast<char16_t>(codepoint);
                return true;
            }
            std::uint32_t value = codepoint - 0x10000;
            unit = static_cast<char16_t>(0xD800 + (value >> 10));
            cursor.pending = static_cast<char16_t>(0xDC00 + (value & 0x3FF));
            cursor.has_pending = true;
            return true;
        }
    }
    return false;
}

struct UnitReader {
    StringCursor cursor;
    bool has_saved = false;
    char16_t saved = 0;
};

bool read_unit(UnitReader &reader, char16_t &unit) noexcept {
    if (reader.has_saved) {
        unit = reader.saved;
        reader.has_saved = false;
        return true;
    }
    return cursor_next(reader.cursor, unit);
}

void unread_unit(UnitReader &reader, char16_t unit) noexcept {
    reader.saved = unit;
    reader.has_saved = true;
}

bool read_next(UnitReader &reader, char16_t &unit, bool &has_unit) noexcept {
    has_unit = read_unit(reader, unit);
    return has_unit || !reader.cursor.malformed;
}

bool drain_remaining(UnitReader &reader) noexcept {
    char16_t unit = 0;
    while (read_unit(reader, unit)) {
    }
    return !reader.cursor.malformed;
}

bool finish_nan(UnitReader &reader, double &out) noexcept {
    if (!drain_remaining(reader)) {
        return false;
    }
    out = std::numeric_limits<double>::quiet_NaN();
    return true;
}

bool trailing_spaces_only(UnitReader &reader, bool &only_spaces) noexcept {
    only_spaces = true;
    char16_t unit = 0;
    while (read_unit(reader, unit)) {
        if (!ascii_is_space(unit)) {
            only_spaces = false;
            return drain_remaining(reader);
        }
    }
    return !reader.cursor.malformed;
}

bool read_ascii_ci(UnitReader &reader, char expected) noexcept {
    char16_t unit = 0;
    return read_unit(reader, unit) && ascii_eq_ci(unit, expected);
}

bool parse_nan(UnitReader &reader, double &out) noexcept {
    if (!read_ascii_ci(reader, 'a') || !read_ascii_ci(reader, 'n')) {
        return finish_nan(reader, out);
    }
    bool only_spaces = true;
    if (!trailing_spaces_only(reader, only_spaces)) {
        return false;
    }
    out = std::numeric_limits<double>::quiet_NaN();
    return true;
}

bool parse_infinity(UnitReader &reader, double sign, double &out) noexcept {
    if (!read_ascii_ci(reader, 'n') || !read_ascii_ci(reader, 'f')) {
        return finish_nan(reader, out);
    }
    char16_t unit = 0;
    bool has_unit = false;
    if (!read_next(reader, unit, has_unit)) {
        return false;
    }
    if (has_unit && ascii_eq_ci(unit, 'i')) {
        if (!read_ascii_ci(reader, 'n') || !read_ascii_ci(reader, 'i') || !read_ascii_ci(reader, 't') ||
            !read_ascii_ci(reader, 'y')) {
            return finish_nan(reader, out);
        }
    } else if (has_unit) {
        unread_unit(reader, unit);
    }
    bool only_spaces = true;
    if (!trailing_spaces_only(reader, only_spaces)) {
        return false;
    }
    if (!only_spaces) {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    out = sign * std::numeric_limits<double>::infinity();
    return true;
}

double scale_decimal(double mantissa, int exp) noexcept {
    if (mantissa == 0.0) {
        return mantissa;
    }
    if (exp > 308) {
        return std::numeric_limits<double>::infinity();
    }
    if (exp < -400) {
        return 0.0;
    }
    return mantissa * std::pow(10.0, static_cast<double>(exp));
}

double scale_binary(double mantissa, int exp) noexcept {
    if (mantissa == 0.0) {
        return mantissa;
    }
    if (exp > 1024) {
        return std::numeric_limits<double>::infinity();
    }
    if (exp < -1100) {
        return 0.0;
    }
    return std::ldexp(mantissa, exp);
}

bool parse_hex_number(UnitReader &reader, double sign, double &out) noexcept {
    constexpr int kMaxSignificantHexDigits = 13;
    double mantissa = 0.0;
    int significant_digits = 0;
    int binary_exp = 0;
    bool saw_digit = false;
    bool saw_nonzero = false;
    auto add_digit = [&](int digit, bool after_dot) noexcept {
        saw_digit = true;
        if (digit != 0 || saw_nonzero) {
            saw_nonzero = true;
            if (significant_digits < kMaxSignificantHexDigits) {
                mantissa = mantissa * 16.0 + static_cast<double>(digit);
                ++significant_digits;
                if (after_dot) {
                    clamp_exp_add_many(binary_exp, -4);
                }
            } else if (!after_dot) {
                clamp_exp_add_many(binary_exp, 4);
            }
        } else if (after_dot) {
            clamp_exp_add_many(binary_exp, -4);
        }
    };

    char16_t unit = 0;
    bool has_unit = false;
    if (!read_next(reader, unit, has_unit)) {
        return false;
    }
    while (has_unit) {
        int digit = ascii_hex_digit(unit);
        if (digit < 0) {
            break;
        }
        add_digit(digit, false);
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
    }
    if (has_unit && unit == '.') {
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
        while (has_unit) {
            int digit = ascii_hex_digit(unit);
            if (digit < 0) {
                break;
            }
            add_digit(digit, true);
            if (!read_next(reader, unit, has_unit)) {
                return false;
            }
        }
    }
    if (!saw_digit) {
        return finish_nan(reader, out);
    }

    int exponent = 0;
    if (has_unit && (unit == 'p' || unit == 'P')) {
        int exponent_sign = 1;
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
        if (has_unit && (unit == '+' || unit == '-')) {
            exponent_sign = unit == '-' ? -1 : 1;
            if (!read_next(reader, unit, has_unit)) {
                return false;
            }
        }
        if (!has_unit || !ascii_is_digit(unit)) {
            return finish_nan(reader, out);
        }
        while (has_unit && ascii_is_digit(unit)) {
            if (exponent < 100000) {
                exponent = exponent * 10 + static_cast<int>(unit - '0');
                if (exponent > 100000) {
                    exponent = 100000;
                }
            }
            if (!read_next(reader, unit, has_unit)) {
                return false;
            }
        }
        exponent *= exponent_sign;
    }

    if (has_unit) {
        if (!ascii_is_space(unit)) {
            return finish_nan(reader, out);
        }
        bool only_spaces = true;
        if (!trailing_spaces_only(reader, only_spaces)) {
            return false;
        }
        if (!only_spaces) {
            out = std::numeric_limits<double>::quiet_NaN();
            return true;
        }
    }
    int total_exp = binary_exp;
    clamp_exp_add_many(total_exp, exponent);
    out = sign * scale_binary(mantissa, total_exp);
    return true;
}

bool string_cursor_to_number(StringCursor cursor, double &out) noexcept {
    constexpr int kMaxSignificantDigits = 18;
    UnitReader reader{cursor};
    char16_t unit = 0;
    bool has_unit = false;
    do {
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
        if (!has_unit) {
            out = 0.0;
            return true;
        }
    } while (ascii_is_space(unit));

    double sign = 1.0;
    if (unit == '+' || unit == '-') {
        sign = unit == '-' ? -1.0 : 1.0;
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
        if (!has_unit) {
            return finish_nan(reader, out);
        }
    }

    if (ascii_eq_ci(unit, 'n')) {
        return parse_nan(reader, out);
    }
    if (ascii_eq_ci(unit, 'i')) {
        return parse_infinity(reader, sign, out);
    }
    if (unit == '0') {
        char16_t next = 0;
        bool has_next = false;
        if (!read_next(reader, next, has_next)) {
            return false;
        }
        if (has_next && (next == 'x' || next == 'X')) {
            return parse_hex_number(reader, sign, out);
        }
        if (has_next) {
            unread_unit(reader, next);
        }
    }

    double mantissa = 0.0;
    int significant_digits = 0;
    int decimal_exp = 0;
    bool saw_digit = false;
    bool saw_nonzero = false;
    auto add_digit = [&](int digit, bool after_dot) noexcept {
        saw_digit = true;
        if (digit != 0 || saw_nonzero) {
            saw_nonzero = true;
            if (significant_digits < kMaxSignificantDigits) {
                mantissa = mantissa * 10.0 + static_cast<double>(digit);
                ++significant_digits;
                if (after_dot) {
                    clamp_exp_add(decimal_exp, -1);
                }
            } else if (!after_dot) {
                clamp_exp_add(decimal_exp, 1);
            }
        } else if (after_dot) {
            clamp_exp_add(decimal_exp, -1);
        }
    };

    while (has_unit && ascii_is_digit(unit)) {
        add_digit(static_cast<int>(unit - '0'), false);
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
    }
    if (has_unit && unit == '.') {
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
        while (has_unit && ascii_is_digit(unit)) {
            add_digit(static_cast<int>(unit - '0'), true);
            if (!read_next(reader, unit, has_unit)) {
                return false;
            }
        }
    }
    if (!saw_digit) {
        return finish_nan(reader, out);
    }

    int exponent = 0;
    if (has_unit && (unit == 'e' || unit == 'E')) {
        int exponent_sign = 1;
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
        if (has_unit && (unit == '+' || unit == '-')) {
            exponent_sign = unit == '-' ? -1 : 1;
            if (!read_next(reader, unit, has_unit)) {
                return false;
            }
        }
        if (!has_unit || !ascii_is_digit(unit)) {
            return finish_nan(reader, out);
        }
        while (has_unit && ascii_is_digit(unit)) {
            if (exponent < 100000) {
                exponent = exponent * 10 + static_cast<int>(unit - '0');
                if (exponent > 100000) {
                    exponent = 100000;
                }
            }
            if (!read_next(reader, unit, has_unit)) {
                return false;
            }
        }
        exponent *= exponent_sign;
    }

    if (has_unit) {
        if (!ascii_is_space(unit)) {
            return finish_nan(reader, out);
        }
        bool only_spaces = true;
        if (!trailing_spaces_only(reader, only_spaces)) {
            return false;
        }
        if (!only_spaces) {
            out = std::numeric_limits<double>::quiet_NaN();
            return true;
        }
    }
    int total_exp = decimal_exp;
    clamp_exp_add_many(total_exp, exponent);
    out = sign * scale_decimal(mantissa, total_exp);
    return true;
}

bool string_to_number(const fiber::script::JsValue &value, double &out) noexcept {
    StringCursor cursor;
    if (!init_string_cursor(value, cursor)) {
        return false;
    }
    return string_cursor_to_number(cursor, out);
}

bool compare_string_cursors(StringCursor lhs_cursor, StringCursor rhs_cursor, int &result) noexcept {
    while (true) {
        char16_t lhs_unit = 0;
        char16_t rhs_unit = 0;
        bool lhs_has = cursor_next(lhs_cursor, lhs_unit);
        bool rhs_has = cursor_next(rhs_cursor, rhs_unit);
        if (lhs_cursor.malformed || rhs_cursor.malformed) {
            return false;
        }
        if (!lhs_has || !rhs_has) {
            if (lhs_has) {
                result = 1;
            } else if (rhs_has) {
                result = -1;
            } else {
                result = 0;
            }
            return true;
        }
        if (lhs_unit < rhs_unit) {
            result = -1;
            return true;
        }
        if (lhs_unit > rhs_unit) {
            result = 1;
            return true;
        }
    }
}

bool compare_strings(const fiber::script::JsValue &lhs, const fiber::script::JsValue &rhs, int &result) noexcept {
    if (!fiber::script::js_value_is_borrowed_string(lhs) && !fiber::script::js_value_is_borrowed_string(rhs) &&
        fiber::script::js_value_type(lhs) == fiber::script::JsNodeType::String &&
        fiber::script::js_value_type(rhs) == fiber::script::JsNodeType::String) {
        auto *lhs_str = as_heap_string(lhs);
        auto *rhs_str = as_heap_string(rhs);
        if (!lhs_str || !rhs_str) {
            return false;
        }
        if (fiber::script::gc_string_is_ascii(lhs_str) && fiber::script::gc_string_is_ascii(rhs_str)) {
            const std::size_t lhs_len = fiber::script::gc_string_byte_len(lhs_str);
            const std::size_t rhs_len = fiber::script::gc_string_byte_len(rhs_str);
            std::size_t min_len = lhs_len < rhs_len ? lhs_len : rhs_len;
            int cmp = 0;
            if (min_len > 0) {
                cmp = std::memcmp(fiber::script::gc_string_wtf8_data(lhs_str),
                                  fiber::script::gc_string_wtf8_data(rhs_str), min_len);
            }
            if (cmp == 0) {
                if (lhs_len < rhs_len) {
                    cmp = -1;
                } else if (lhs_len > rhs_len) {
                    cmp = 1;
                }
            }
            result = cmp;
            return true;
        }
    }

    StringCursor lhs_cursor;
    StringCursor rhs_cursor;
    if (!init_string_cursor(lhs, lhs_cursor) || !init_string_cursor(rhs, rhs_cursor)) {
        return false;
    }
    return compare_string_cursors(lhs_cursor, rhs_cursor, result);
}

bool heap_string_equals_native_utf8(const fiber::script::GcString *heap_str, fiber::script::NativeStr native) noexcept {
    StringCursor lhs_cursor;
    StringCursor rhs_cursor;
    if (!init_heap_string_cursor(heap_str, lhs_cursor) || !init_native_string_cursor(native, rhs_cursor)) {
        return false;
    }
    int cmp = 0;
    return compare_string_cursors(lhs_cursor, rhs_cursor, cmp) && cmp == 0;
}

double number_value(const fiber::script::JsValue &value) noexcept {
    return fiber::script::js_value_type(value) == fiber::script::JsNodeType::Integer
                   ? static_cast<double>(fiber::script::js_value_int64(value))
                   : fiber::script::js_value_double(value);
}

bool numbers_equal(double lhs, double rhs) noexcept {
    if (std::isnan(lhs) || std::isnan(rhs)) {
        return false;
    }
    return lhs == rhs;
}

bool strict_equal(const fiber::script::JsValue &lhs, const fiber::script::JsValue &rhs) noexcept {
    if (is_string_type(fiber::script::js_value_type(lhs)) && is_string_type(fiber::script::js_value_type(rhs))) {
        int cmp = 0;
        if (!compare_strings(lhs, rhs, cmp)) {
            return false;
        }
        return cmp == 0;
    }
    if (is_number_type(fiber::script::js_value_type(lhs)) && is_number_type(fiber::script::js_value_type(rhs))) {
        return numbers_equal(number_value(lhs), number_value(rhs));
    }
    if (fiber::script::js_value_type(lhs) != fiber::script::js_value_type(rhs)) {
        return false;
    }
    switch (fiber::script::js_value_type(lhs)) {
        case fiber::script::JsNodeType::Undefined:
        case fiber::script::JsNodeType::Null:
            return true;
        case fiber::script::JsNodeType::Boolean:
            return fiber::script::js_value_bool(lhs) == fiber::script::js_value_bool(rhs);
        case fiber::script::JsNodeType::Integer:
            return fiber::script::js_value_int64(lhs) == fiber::script::js_value_int64(rhs);
        case fiber::script::JsNodeType::Float:
            return fiber::script::js_value_double(lhs) == fiber::script::js_value_double(rhs);
        case fiber::script::JsNodeType::Binary:
            if (fiber::script::js_value_is_borrowed_binary(lhs) || fiber::script::js_value_is_borrowed_binary(rhs)) {
                fiber::script::NativeBin lhs_bin = fiber::script::js_value_native_binary(lhs);
                fiber::script::NativeBin rhs_bin = fiber::script::js_value_native_binary(rhs);
                return lhs_bin.data == rhs_bin.data && lhs_bin.len == rhs_bin.len;
            }
            return fiber::script::js_value_heap_header(lhs) == fiber::script::js_value_heap_header(rhs);
        case fiber::script::JsNodeType::Array:
        case fiber::script::JsNodeType::Object:
        case fiber::script::JsNodeType::Interator:
        case fiber::script::JsNodeType::Exception:
            return fiber::script::js_value_heap_header(lhs) == fiber::script::js_value_heap_header(rhs);
        case fiber::script::JsNodeType::String:
            break;
    }
    return false;
}

bool loose_equal_number(double number, const fiber::script::JsValue &other) noexcept {
    if (is_number_type(fiber::script::js_value_type(other))) {
        return numbers_equal(number, number_value(other));
    }
    if (is_string_type(fiber::script::js_value_type(other))) {
        double other_number = 0.0;
        if (!string_to_number(other, other_number)) {
            return false;
        }
        return numbers_equal(number, other_number);
    }
    return false;
}

bool loose_equal(const fiber::script::JsValue &lhs, const fiber::script::JsValue &rhs) noexcept {
    if (is_string_type(fiber::script::js_value_type(lhs)) && is_string_type(fiber::script::js_value_type(rhs))) {
        int cmp = 0;
        if (!compare_strings(lhs, rhs, cmp)) {
            return false;
        }
        return cmp == 0;
    }
    if (is_number_type(fiber::script::js_value_type(lhs)) && is_number_type(fiber::script::js_value_type(rhs))) {
        return numbers_equal(number_value(lhs), number_value(rhs));
    }
    if (fiber::script::js_value_type(lhs) == fiber::script::js_value_type(rhs)) {
        return strict_equal(lhs, rhs);
    }
    if ((fiber::script::js_value_type(lhs) == fiber::script::JsNodeType::Null &&
         fiber::script::js_value_type(rhs) == fiber::script::JsNodeType::Undefined) ||
        (fiber::script::js_value_type(lhs) == fiber::script::JsNodeType::Undefined &&
         fiber::script::js_value_type(rhs) == fiber::script::JsNodeType::Null)) {
        return true;
    }
    if (fiber::script::js_value_type(lhs) == fiber::script::JsNodeType::Boolean) {
        double lhs_number = fiber::script::js_value_bool(lhs) ? 1.0 : 0.0;
        return loose_equal_number(lhs_number, rhs);
    }
    if (fiber::script::js_value_type(rhs) == fiber::script::JsNodeType::Boolean) {
        double rhs_number = fiber::script::js_value_bool(rhs) ? 1.0 : 0.0;
        return loose_equal_number(rhs_number, lhs);
    }
    if (is_number_type(fiber::script::js_value_type(lhs)) && is_string_type(fiber::script::js_value_type(rhs))) {
        double rhs_number = 0.0;
        if (!string_to_number(rhs, rhs_number)) {
            return false;
        }
        return numbers_equal(number_value(lhs), rhs_number);
    }
    if (is_string_type(fiber::script::js_value_type(lhs)) && is_number_type(fiber::script::js_value_type(rhs))) {
        double lhs_number = 0.0;
        if (!string_to_number(lhs, lhs_number)) {
            return false;
        }
        return numbers_equal(lhs_number, number_value(rhs));
    }
    return false;
}

bool equality(ConstValueHandle a, ConstValueHandle b, bool strict, bool invert) noexcept {
    bool equal = strict ? strict_equal(*a, *b) : loose_equal(*a, *b);
    return invert ? !equal : equal;
}

bool relation_result(double lhs, double rhs, bool less_direction, bool allow_equal) noexcept {
    if (std::isnan(lhs) || std::isnan(rhs)) {
        return false;
    }
    if (less_direction) {
        return allow_equal ? lhs <= rhs : lhs < rhs;
    }
    return allow_equal ? lhs >= rhs : lhs > rhs;
}

bool relation_result(int cmp, bool less_direction, bool allow_equal) noexcept {
    if (less_direction) {
        return allow_equal ? cmp <= 0 : cmp < 0;
    }
    return allow_equal ? cmp >= 0 : cmp > 0;
}

bool relation(ConstValueHandle a, ConstValueHandle b, bool less_direction, bool allow_equal) noexcept {
    if (is_string_type(fiber::script::js_value_type(*a)) && is_string_type(fiber::script::js_value_type(*b))) {
        int cmp = 0;
        if (!compare_strings(*a, *b, cmp)) {
            return false;
        }
        return relation_result(cmp, less_direction, allow_equal);
    }
    if (!is_numeric_like(fiber::script::js_value_type(*a)) || !is_numeric_like(fiber::script::js_value_type(*b))) {
        return false;
    }
    double lhs = 0.0;
    double rhs = 0.0;
    if (!to_number(*a, lhs) || !to_number(*b, rhs)) {
        return false;
    }
    return relation_result(lhs, rhs, less_direction, allow_equal);
}

} // namespace

bool Compares::neg(ConstValueHandle value) noexcept { return !logic(value); }

bool Compares::logic(ConstValueHandle value) noexcept {
    if (!value) {
        return false;
    }
    return is_truthy(*value);
}

bool Compares::eq(ConstValueHandle a, ConstValueHandle b) noexcept { return equality(a, b, false, false); }

bool Compares::seq(ConstValueHandle a, ConstValueHandle b) noexcept { return equality(a, b, true, false); }

bool Compares::ne(ConstValueHandle a, ConstValueHandle b) noexcept { return equality(a, b, false, true); }

bool Compares::sne(ConstValueHandle a, ConstValueHandle b) noexcept { return equality(a, b, true, true); }

bool Compares::lt(ConstValueHandle a, ConstValueHandle b) noexcept { return relation(a, b, true, false); }

bool Compares::lte(ConstValueHandle a, ConstValueHandle b) noexcept { return relation(a, b, true, true); }

bool Compares::gt(ConstValueHandle a, ConstValueHandle b) noexcept { return relation(a, b, false, false); }

bool Compares::gte(ConstValueHandle a, ConstValueHandle b) noexcept { return relation(a, b, false, true); }

bool Compares::matches(ConstValueHandle a, ConstValueHandle b) noexcept {
    (void) a;
    (void) b;
    return false;
}

bool Compares::in(ConstValueHandle a, ConstValueHandle b) noexcept {
    if (!a || !b) {
        return false;
    }
    if (fiber::script::js_value_type(*b) == fiber::script::JsNodeType::Array) {
        if (fiber::script::js_value_type(*a) != fiber::script::JsNodeType::Integer) {
            return false;
        }
        auto *arr = fiber::script::js_value_heap_ptr<const fiber::script::GcArray>(*b);
        std::int64_t index = fiber::script::js_value_int64(*a);
        if (!arr || index < 0) {
            return false;
        }
        return static_cast<std::size_t>(index) < arr->size;
    }
    if (fiber::script::js_value_type(*b) == fiber::script::JsNodeType::Object) {
        auto *obj = fiber::script::js_value_heap_ptr<const fiber::script::GcObject>(*b);
        if (!obj) {
            return false;
        }
        if (fiber::script::js_value_type(*a) == fiber::script::JsNodeType::String &&
            !fiber::script::js_value_is_borrowed_string(*a)) {
            auto *key_str = fiber::script::js_value_heap_ptr<const fiber::script::GcString>(*a);
            const fiber::script::JsValue *found = fiber::script::gc_object_get(obj, key_str);
            return found != nullptr;
        }
        if (fiber::script::js_value_type(*a) == fiber::script::JsNodeType::String &&
            fiber::script::js_value_is_borrowed_string(*a)) {
            fiber::script::NativeStr native = fiber::script::js_value_native_string(*a);
            for (const fiber::script::GcObjectEntry *entry = fiber::script::gc_object_first_entry(obj);
                 entry != nullptr; entry = fiber::script::gc_object_next_entry(obj, entry)) {
                if (!entry->occupied || !entry->key) {
                    continue;
                }
                if (heap_string_equals_native_utf8(entry->key, native)) {
                    return true;
                }
            }
        }
        return false;
    }
    return false;
}

} // namespace fiber::script::run
