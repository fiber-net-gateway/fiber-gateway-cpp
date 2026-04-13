//
// Created by dear on 2025/12/30.
//

#include "JsValueOps.h"

#include "Utf.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace fiber::json {

namespace {

JsOpResult make_error(JsOpError error) {
    JsOpResult result;
    result.value = JsValue::make_undefined();
    result.error = error;
    return result;
}

bool is_string_type(JsNodeType type) { return type == JsNodeType::String; }

bool is_number_type(JsNodeType type) { return type == JsNodeType::Integer || type == JsNodeType::Float; }

bool is_numeric_like(JsNodeType type) {
    return type == JsNodeType::Integer || type == JsNodeType::Float || type == JsNodeType::Boolean ||
           type == JsNodeType::Null;
}

const GcString *as_heap_string(const JsValue &value) {
    return js_value_type(value) == JsNodeType::String ? js_value_heap_ptr<const GcString>(value) : nullptr;
}

const GcBinary *as_heap_binary(const JsValue &value) {
    return js_value_type(value) == JsNodeType::Binary ? js_value_heap_ptr<const GcBinary>(value) : nullptr;
}

bool to_number(const JsValue &value, double &out) {
    switch (js_value_type(value)) {
        case JsNodeType::Integer:
            out = static_cast<double>(js_value_int64(value));
            return true;
        case JsNodeType::Float:
            out = js_value_double(value);
            return true;
        case JsNodeType::Boolean:
            out = js_value_bool(value) ? 1.0 : 0.0;
            return true;
        case JsNodeType::Null:
            out = 0.0;
            return true;
        default:
            return false;
    }
}

bool to_int64(const JsValue &value, std::int64_t &out) {
    switch (js_value_type(value)) {
        case JsNodeType::Integer:
            out = js_value_int64(value);
            return true;
        case JsNodeType::Float:
            out = static_cast<std::int64_t>(js_value_double(value));
            return true;
        case JsNodeType::Boolean:
            out = js_value_bool(value) ? 1 : 0;
            return true;
        case JsNodeType::Null:
            out = 0;
            return true;
        default:
            return false;
    }
}

bool is_truthy(const JsValue &value) {
    switch (js_value_type(value)) {
        case JsNodeType::Undefined:
        case JsNodeType::Null:
            return false;
        case JsNodeType::Boolean:
            return js_value_bool(value);
        case JsNodeType::Integer:
            return js_value_int64(value) != 0;
        case JsNodeType::Float:
            return js_value_double(value) != 0.0 && !std::isnan(js_value_double(value));
        case JsNodeType::String:
            if (js_value_is_borrowed_string(value)) {
                return js_value_native_string(value).len > 0;
            }
            return as_heap_string(value) != nullptr && as_heap_string(value)->len > 0;
        case JsNodeType::Binary:
        case JsNodeType::Array:
        case JsNodeType::Object:
        case JsNodeType::Interator:
        case JsNodeType::Exception:
            return true;
    }
    return false;
}

enum class StringKind : std::uint8_t {
    HeapByte,
    HeapUtf16,
    NativeUtf8,
};

struct StringSource {
    StringKind kind = StringKind::NativeUtf8;
    const std::uint8_t *bytes = nullptr;
    const char16_t *u16 = nullptr;
    const char *utf8 = nullptr;
    std::size_t len = 0;
    Utf8ScanResult scan = {};
};

bool build_string_source(const JsValue &value, StringSource &out, JsOpError &error) {
    if (js_value_type(value) != JsNodeType::String) {
        error = JsOpError::TypeError;
        return false;
    }
    if (!js_value_is_borrowed_string(value)) {
        auto *str = as_heap_string(value);
        if (!str) {
            error = JsOpError::TypeError;
            return false;
        }
        if (str->encoding == GcStringEncoding::Byte) {
            out.kind = StringKind::HeapByte;
            out.bytes = str->data8;
            out.len = str->len;
        } else {
            out.kind = StringKind::HeapUtf16;
            out.u16 = str->data16;
            out.len = str->len;
        }
        return true;
    }
    out.kind = StringKind::NativeUtf8;
    out.utf8 = js_value_native_string(value).data;
    out.len = js_value_native_string(value).len;
    if (!utf8_scan(out.utf8, out.len, out.scan)) {
        error = JsOpError::InvalidUtf8;
        return false;
    }
    return true;
}

bool concat_strings(GcHeap *heap, const StringSource &lhs, const StringSource &rhs, JsValue &out, JsOpError &error) {
    if (!heap) {
        error = JsOpError::HeapRequired;
        return false;
    }
    bool all_byte = true;
    std::size_t total_len = 0;
    auto add_part = [&](const StringSource &part) {
        switch (part.kind) {
            case StringKind::HeapByte:
                total_len += part.len;
                break;
            case StringKind::HeapUtf16:
                all_byte = false;
                total_len += part.len;
                break;
            case StringKind::NativeUtf8:
                if (!part.scan.all_byte) {
                    all_byte = false;
                }
                total_len += part.scan.utf16_len;
                break;
        }
    };
    add_part(lhs);
    add_part(rhs);

    if (total_len == 0) {
        out = JsValue::make_string(*heap, "", 0);
        if (js_value_type(out) != JsNodeType::String || js_value_is_borrowed_string(out)) {
            error = JsOpError::OutOfMemory;
            return false;
        }
        return true;
    }

    if (all_byte) {
        GcString *result = gc_new_string_bytes_uninit(heap, total_len);
        if (!result) {
            error = JsOpError::OutOfMemory;
            return false;
        }
        std::uint8_t *dst = result->data8;
        std::size_t offset = 0;
        auto append_part = [&](const StringSource &part) -> bool {
            switch (part.kind) {
                case StringKind::HeapByte:
                    if (part.len > 0 && part.bytes) {
                        std::memcpy(dst + offset, part.bytes, part.len);
                    }
                    offset += part.len;
                    return true;
                case StringKind::NativeUtf8:
                    if (!utf8_write_bytes(part.utf8, part.len, dst + offset, part.scan.utf16_len)) {
                        return false;
                    }
                    offset += part.scan.utf16_len;
                    return true;
                case StringKind::HeapUtf16:
                    return false;
            }
            return false;
        };
        if (!append_part(lhs) || !append_part(rhs)) {
            error = JsOpError::InvalidUtf8;
            return false;
        }
        out = js_make_heap_ref(&result->hdr, JsHeapKind::String);
        return true;
    }

    GcString *result = gc_new_string_utf16_uninit(heap, total_len);
    if (!result) {
        error = JsOpError::OutOfMemory;
        return false;
    }
    char16_t *dst = result->data16;
    std::size_t offset = 0;
    auto append_part = [&](const StringSource &part) -> bool {
        switch (part.kind) {
            case StringKind::HeapUtf16:
                if (part.len > 0 && part.u16) {
                    std::memcpy(dst + offset, part.u16, sizeof(char16_t) * part.len);
                }
                offset += part.len;
                return true;
            case StringKind::HeapByte:
                for (std::size_t i = 0; i < part.len; ++i) {
                    dst[offset++] = static_cast<char16_t>(part.bytes[i]);
                }
                return true;
            case StringKind::NativeUtf8:
                if (!utf8_write_utf16(part.utf8, part.len, dst + offset, part.scan.utf16_len)) {
                    return false;
                }
                offset += part.scan.utf16_len;
                return true;
        }
        return false;
    };
    if (!append_part(lhs) || !append_part(rhs)) {
        error = JsOpError::InvalidUtf8;
        return false;
    }
    out = js_make_heap_ref(&result->hdr, JsHeapKind::String);
    return true;
}

bool string_to_utf8_copy(const JsValue &value, std::string &out, JsOpError &error) {
    out.clear();
    if (js_value_type(value) != JsNodeType::String) {
        error = JsOpError::TypeError;
        return false;
    }
    if (js_value_is_borrowed_string(value)) {
        if (!utf8_validate(js_value_native_string(value).data, js_value_native_string(value).len)) {
            error = JsOpError::InvalidUtf8;
            return false;
        }
        if (js_value_native_string(value).len == 0) {
            return true;
        }
        out.assign(js_value_native_string(value).data, js_value_native_string(value).len);
        return true;
    }
    auto *str = as_heap_string(value);
    if (!gc_string_to_utf8(str, out)) {
        error = JsOpError::InvalidUtf8;
        return false;
    }
    return true;
}

bool ascii_is_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v'; }

bool string_to_number(const JsValue &value, double &out, JsOpError &error) {
    std::string buffer;
    if (!string_to_utf8_copy(value, buffer, error)) {
        return false;
    }
    std::size_t start = 0;
    std::size_t end = buffer.size();
    while (start < end && ascii_is_space(buffer[start])) {
        start += 1;
    }
    while (end > start && ascii_is_space(buffer[end - 1])) {
        end -= 1;
    }
    if (start == end) {
        out = 0.0;
        return true;
    }
    std::string view = buffer.substr(start, end - start);
    char *end_ptr = nullptr;
    out = std::strtod(view.c_str(), &end_ptr);
    if (end_ptr != view.c_str() + view.size()) {
        out = std::numeric_limits<double>::quiet_NaN();
    }
    return true;
}

struct StringCursor {
    StringKind kind = StringKind::NativeUtf8;
    const std::uint8_t *bytes = nullptr;
    const char16_t *u16 = nullptr;
    const char *utf8 = nullptr;
    std::size_t len = 0;
    std::size_t index = 0;
    std::size_t pos = 0;
    bool has_pending = false;
    char16_t pending = 0;
};

bool init_string_cursor(const JsValue &value, StringCursor &out, JsOpError &error) {
    if (js_value_type(value) != JsNodeType::String) {
        error = JsOpError::TypeError;
        return false;
    }
    if (!js_value_is_borrowed_string(value)) {
        auto *str = as_heap_string(value);
        if (!str) {
            error = JsOpError::TypeError;
            return false;
        }
        if (str->encoding == GcStringEncoding::Byte) {
            out.kind = StringKind::HeapByte;
            out.bytes = str->data8;
        } else {
            out.kind = StringKind::HeapUtf16;
            out.u16 = str->data16;
        }
        out.len = str->len;
        if (out.len > 0 && !out.bytes && !out.u16) {
            error = JsOpError::TypeError;
            return false;
        }
        return true;
    }
    out.kind = StringKind::NativeUtf8;
    out.utf8 = js_value_native_string(value).data;
    out.len = js_value_native_string(value).len;
    if (out.len > 0 && !out.utf8) {
        error = JsOpError::InvalidUtf8;
        return false;
    }
    return true;
}

bool cursor_next(StringCursor &cursor, char16_t &unit, JsOpError &error) {
    if (cursor.has_pending) {
        unit = cursor.pending;
        cursor.has_pending = false;
        return true;
    }
    switch (cursor.kind) {
        case StringKind::HeapByte:
            if (cursor.index >= cursor.len) {
                return false;
            }
            unit = static_cast<char16_t>(cursor.bytes[cursor.index++]);
            return true;
        case StringKind::HeapUtf16:
            if (cursor.index >= cursor.len) {
                return false;
            }
            unit = cursor.u16[cursor.index++];
            return true;
        case StringKind::NativeUtf8: {
            if (cursor.pos >= cursor.len) {
                return false;
            }
            std::uint32_t codepoint = 0;
            if (!utf8_next_codepoint(cursor.utf8, cursor.len, cursor.pos, codepoint)) {
                error = JsOpError::InvalidUtf8;
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
    error = JsOpError::TypeError;
    return false;
}

bool compare_strings(const JsValue &lhs, const JsValue &rhs, int &result, JsOpError &error) {
    if (!js_value_is_borrowed_string(lhs) && !js_value_is_borrowed_string(rhs) &&
        js_value_type(lhs) == JsNodeType::String && js_value_type(rhs) == JsNodeType::String) {
        auto *lhs_str = as_heap_string(lhs);
        auto *rhs_str = as_heap_string(rhs);
        if (!lhs_str || !rhs_str) {
            error = JsOpError::TypeError;
            return false;
        }
        if (lhs_str->encoding == GcStringEncoding::Byte && rhs_str->encoding == GcStringEncoding::Byte) {
            std::size_t min_len = lhs_str->len < rhs_str->len ? lhs_str->len : rhs_str->len;
            int cmp = 0;
            if (min_len > 0) {
                cmp = std::memcmp(lhs_str->data8, rhs_str->data8, min_len);
            }
            if (cmp == 0) {
                if (lhs_str->len < rhs_str->len) {
                    cmp = -1;
                } else if (lhs_str->len > rhs_str->len) {
                    cmp = 1;
                }
            }
            result = cmp;
            return true;
        }
        if (lhs_str->encoding == GcStringEncoding::Utf16 && rhs_str->encoding == GcStringEncoding::Utf16) {
            std::size_t min_len = lhs_str->len < rhs_str->len ? lhs_str->len : rhs_str->len;
            for (std::size_t i = 0; i < min_len; ++i) {
                char16_t l_unit = lhs_str->data16[i];
                char16_t r_unit = rhs_str->data16[i];
                if (l_unit < r_unit) {
                    result = -1;
                    return true;
                }
                if (l_unit > r_unit) {
                    result = 1;
                    return true;
                }
            }
            if (lhs_str->len < rhs_str->len) {
                result = -1;
            } else if (lhs_str->len > rhs_str->len) {
                result = 1;
            } else {
                result = 0;
            }
            return true;
        }
    }

    StringCursor lhs_cursor;
    StringCursor rhs_cursor;
    if (!init_string_cursor(lhs, lhs_cursor, error)) {
        return false;
    }
    if (!init_string_cursor(rhs, rhs_cursor, error)) {
        return false;
    }
    while (true) {
        char16_t lhs_unit = 0;
        char16_t rhs_unit = 0;
        bool lhs_has = cursor_next(lhs_cursor, lhs_unit, error);
        if (error != JsOpError::None) {
            return false;
        }
        bool rhs_has = cursor_next(rhs_cursor, rhs_unit, error);
        if (error != JsOpError::None) {
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

double number_value(const JsValue &value) {
    return js_value_type(value) == JsNodeType::Integer ? static_cast<double>(js_value_int64(value))
                                                       : js_value_double(value);
}

bool numbers_equal(double lhs, double rhs) {
    if (std::isnan(lhs) || std::isnan(rhs)) {
        return false;
    }
    return lhs == rhs;
}

bool strict_equal(const JsValue &lhs, const JsValue &rhs, JsOpError &error) {
    if (is_string_type(js_value_type(lhs)) && is_string_type(js_value_type(rhs))) {
        int cmp = 0;
        if (!compare_strings(lhs, rhs, cmp, error)) {
            return false;
        }
        return cmp == 0;
    }
    if (is_number_type(js_value_type(lhs)) && is_number_type(js_value_type(rhs))) {
        return numbers_equal(number_value(lhs), number_value(rhs));
    }
    if (js_value_type(lhs) != js_value_type(rhs)) {
        return false;
    }
    switch (js_value_type(lhs)) {
        case JsNodeType::Undefined:
        case JsNodeType::Null:
            return true;
        case JsNodeType::Boolean:
            return js_value_bool(lhs) == js_value_bool(rhs);
        case JsNodeType::Integer:
            return js_value_int64(lhs) == js_value_int64(rhs);
        case JsNodeType::Float:
            return js_value_double(lhs) == js_value_double(rhs);
        case JsNodeType::Binary:
            if (js_value_is_borrowed_binary(lhs) || js_value_is_borrowed_binary(rhs)) {
                NativeBin lhs_bin = js_value_native_binary(lhs);
                NativeBin rhs_bin = js_value_native_binary(rhs);
                return lhs_bin.data == rhs_bin.data && lhs_bin.len == rhs_bin.len;
            }
            return js_value_heap_header(lhs) == js_value_heap_header(rhs);
        case JsNodeType::Array:
        case JsNodeType::Object:
        case JsNodeType::Interator:
        case JsNodeType::Exception:
            return js_value_heap_header(lhs) == js_value_heap_header(rhs);
        case JsNodeType::String:
            break;
    }
    return false;
}

bool loose_equal_number(double number, const JsValue &other, JsOpError &error) {
    if (is_number_type(js_value_type(other))) {
        return numbers_equal(number, number_value(other));
    }
    if (is_string_type(js_value_type(other))) {
        double other_number = 0.0;
        if (!string_to_number(other, other_number, error)) {
            return false;
        }
        return numbers_equal(number, other_number);
    }
    return false;
}

bool loose_equal(const JsValue &lhs, const JsValue &rhs, JsOpError &error) {
    if (is_string_type(js_value_type(lhs)) && is_string_type(js_value_type(rhs))) {
        int cmp = 0;
        if (!compare_strings(lhs, rhs, cmp, error)) {
            return false;
        }
        return cmp == 0;
    }
    if (is_number_type(js_value_type(lhs)) && is_number_type(js_value_type(rhs))) {
        return numbers_equal(number_value(lhs), number_value(rhs));
    }
    if (js_value_type(lhs) == js_value_type(rhs)) {
        return strict_equal(lhs, rhs, error);
    }
    if ((js_value_type(lhs) == JsNodeType::Null && js_value_type(rhs) == JsNodeType::Undefined) ||
        (js_value_type(lhs) == JsNodeType::Undefined && js_value_type(rhs) == JsNodeType::Null)) {
        return true;
    }
    if (js_value_type(lhs) == JsNodeType::Boolean) {
        double lhs_number = js_value_bool(lhs) ? 1.0 : 0.0;
        return loose_equal_number(lhs_number, rhs, error);
    }
    if (js_value_type(rhs) == JsNodeType::Boolean) {
        double rhs_number = js_value_bool(rhs) ? 1.0 : 0.0;
        return loose_equal_number(rhs_number, lhs, error);
    }
    if (is_number_type(js_value_type(lhs)) && is_string_type(js_value_type(rhs))) {
        double rhs_number = 0.0;
        if (!string_to_number(rhs, rhs_number, error)) {
            return false;
        }
        return numbers_equal(number_value(lhs), rhs_number);
    }
    if (is_string_type(js_value_type(lhs)) && is_number_type(js_value_type(rhs))) {
        double lhs_number = 0.0;
        if (!string_to_number(lhs, lhs_number, error)) {
            return false;
        }
        return numbers_equal(lhs_number, number_value(rhs));
    }
    return false;
}

JsOpResult add_numeric(const JsValue &lhs, const JsValue &rhs) {
    if (!is_numeric_like(js_value_type(lhs)) || !is_numeric_like(js_value_type(rhs))) {
        return make_error(JsOpError::TypeError);
    }
    if (js_value_type(lhs) == JsNodeType::Float || js_value_type(rhs) == JsNodeType::Float) {
        double a = 0.0;
        double b = 0.0;
        if (!to_number(lhs, a) || !to_number(rhs, b)) {
            return make_error(JsOpError::TypeError);
        }
        JsOpResult result;
        result.value = JsValue::make_float(a + b);
        return result;
    }
    std::int64_t a = 0;
    std::int64_t b = 0;
    if (!to_int64(lhs, a) || !to_int64(rhs, b)) {
        return make_error(JsOpError::TypeError);
    }
    std::int64_t out = 0;
    if (!__builtin_add_overflow(a, b, &out)) {
        JsOpResult result;
        result.value = JsValue::make_integer(out);
        return result;
    }
    JsOpResult result;
    result.value = JsValue::make_float(static_cast<double>(a) + static_cast<double>(b));
    return result;
}

JsOpResult sub_numeric(const JsValue &lhs, const JsValue &rhs) {
    if (!is_numeric_like(js_value_type(lhs)) || !is_numeric_like(js_value_type(rhs))) {
        return make_error(JsOpError::TypeError);
    }
    if (js_value_type(lhs) == JsNodeType::Float || js_value_type(rhs) == JsNodeType::Float) {
        double a = 0.0;
        double b = 0.0;
        if (!to_number(lhs, a) || !to_number(rhs, b)) {
            return make_error(JsOpError::TypeError);
        }
        JsOpResult result;
        result.value = JsValue::make_float(a - b);
        return result;
    }
    std::int64_t a = 0;
    std::int64_t b = 0;
    if (!to_int64(lhs, a) || !to_int64(rhs, b)) {
        return make_error(JsOpError::TypeError);
    }
    std::int64_t out = 0;
    if (!__builtin_sub_overflow(a, b, &out)) {
        JsOpResult result;
        result.value = JsValue::make_integer(out);
        return result;
    }
    JsOpResult result;
    result.value = JsValue::make_float(static_cast<double>(a) - static_cast<double>(b));
    return result;
}

JsOpResult mul_numeric(const JsValue &lhs, const JsValue &rhs) {
    if (!is_numeric_like(js_value_type(lhs)) || !is_numeric_like(js_value_type(rhs))) {
        return make_error(JsOpError::TypeError);
    }
    if (js_value_type(lhs) == JsNodeType::Float || js_value_type(rhs) == JsNodeType::Float) {
        double a = 0.0;
        double b = 0.0;
        if (!to_number(lhs, a) || !to_number(rhs, b)) {
            return make_error(JsOpError::TypeError);
        }
        JsOpResult result;
        result.value = JsValue::make_float(a * b);
        return result;
    }
    std::int64_t a = 0;
    std::int64_t b = 0;
    if (!to_int64(lhs, a) || !to_int64(rhs, b)) {
        return make_error(JsOpError::TypeError);
    }
    std::int64_t out = 0;
    if (!__builtin_mul_overflow(a, b, &out)) {
        JsOpResult result;
        result.value = JsValue::make_integer(out);
        return result;
    }
    JsOpResult result;
    result.value = JsValue::make_float(static_cast<double>(a) * static_cast<double>(b));
    return result;
}

} // namespace

JsOpResult js_unary_op(JsUnaryOp op, const JsValue &value) {
    switch (op) {
        case JsUnaryOp::Plus: {
            if (!is_numeric_like(js_value_type(value))) {
                return make_error(JsOpError::TypeError);
            }
            if (js_value_type(value) == JsNodeType::Float) {
                JsOpResult result;
                result.value = JsValue::make_float(js_value_double(value));
                return result;
            }
            std::int64_t int_value = 0;
            if (!to_int64(value, int_value)) {
                return make_error(JsOpError::TypeError);
            }
            JsOpResult result;
            result.value = JsValue::make_integer(int_value);
            return result;
        }
        case JsUnaryOp::Negate: {
            if (!is_numeric_like(js_value_type(value))) {
                return make_error(JsOpError::TypeError);
            }
            if (js_value_type(value) == JsNodeType::Float) {
                JsOpResult result;
                result.value = JsValue::make_float(-js_value_double(value));
                return result;
            }
            std::int64_t int_value = 0;
            if (!to_int64(value, int_value)) {
                return make_error(JsOpError::TypeError);
            }
            if (int_value == std::numeric_limits<std::int64_t>::min()) {
                JsOpResult result;
                result.value = JsValue::make_float(-static_cast<double>(int_value));
                return result;
            }
            JsOpResult result;
            result.value = JsValue::make_integer(-int_value);
            return result;
        }
        case JsUnaryOp::LogicalNot: {
            JsOpResult result;
            result.value = JsValue::make_boolean(!is_truthy(value));
            return result;
        }
    }
    return make_error(JsOpError::TypeError);
}

JsOpResult js_binary_op(JsBinaryOp op, const JsValue &lhs, const JsValue &rhs, GcHeap *heap) {
    switch (op) {
        case JsBinaryOp::LogicalAnd: {
            JsOpResult result;
            result.value = is_truthy(lhs) ? rhs : lhs;
            return result;
        }
        case JsBinaryOp::LogicalOr: {
            JsOpResult result;
            result.value = is_truthy(lhs) ? lhs : rhs;
            return result;
        }
        case JsBinaryOp::Add: {
            if (is_string_type(js_value_type(lhs)) || is_string_type(js_value_type(rhs))) {
                if (!is_string_type(js_value_type(lhs)) || !is_string_type(js_value_type(rhs))) {
                    return make_error(JsOpError::TypeError);
                }
                StringSource lhs_src;
                StringSource rhs_src;
                JsOpError error = JsOpError::None;
                if (!build_string_source(lhs, lhs_src, error)) {
                    return make_error(error);
                }
                if (!build_string_source(rhs, rhs_src, error)) {
                    return make_error(error);
                }
                JsValue out;
                if (!concat_strings(heap, lhs_src, rhs_src, out, error)) {
                    return make_error(error);
                }
                JsOpResult result;
                result.value = std::move(out);
                return result;
            }
            return add_numeric(lhs, rhs);
        }
        case JsBinaryOp::Sub:
            return sub_numeric(lhs, rhs);
        case JsBinaryOp::Mul:
            return mul_numeric(lhs, rhs);
        case JsBinaryOp::Div: {
            if (!is_numeric_like(js_value_type(lhs)) || !is_numeric_like(js_value_type(rhs))) {
                return make_error(JsOpError::TypeError);
            }
            double a = 0.0;
            double b = 0.0;
            if (!to_number(lhs, a) || !to_number(rhs, b)) {
                return make_error(JsOpError::TypeError);
            }
            if (b == 0.0) {
                return make_error(JsOpError::DivisionByZero);
            }
            JsOpResult result;
            result.value = JsValue::make_float(a / b);
            return result;
        }
        case JsBinaryOp::Mod: {
            if (!is_numeric_like(js_value_type(lhs)) || !is_numeric_like(js_value_type(rhs))) {
                return make_error(JsOpError::TypeError);
            }
            if (js_value_type(lhs) == JsNodeType::Float || js_value_type(rhs) == JsNodeType::Float) {
                double a = 0.0;
                double b = 0.0;
                if (!to_number(lhs, a) || !to_number(rhs, b)) {
                    return make_error(JsOpError::TypeError);
                }
                if (b == 0.0) {
                    return make_error(JsOpError::DivisionByZero);
                }
                JsOpResult result;
                result.value = JsValue::make_float(std::fmod(a, b));
                return result;
            }
            std::int64_t a = 0;
            std::int64_t b = 0;
            if (!to_int64(lhs, a) || !to_int64(rhs, b)) {
                return make_error(JsOpError::TypeError);
            }
            if (b == 0) {
                return make_error(JsOpError::DivisionByZero);
            }
            JsOpResult result;
            result.value = JsValue::make_integer(a % b);
            return result;
        }
        case JsBinaryOp::Eq:
        case JsBinaryOp::Ne: {
            JsOpError error = JsOpError::None;
            bool equal = loose_equal(lhs, rhs, error);
            if (error != JsOpError::None) {
                return make_error(error);
            }
            JsOpResult result;
            result.value = JsValue::make_boolean(op == JsBinaryOp::Eq ? equal : !equal);
            return result;
        }
        case JsBinaryOp::StrictEq:
        case JsBinaryOp::StrictNe: {
            JsOpError error = JsOpError::None;
            bool equal = strict_equal(lhs, rhs, error);
            if (error != JsOpError::None) {
                return make_error(error);
            }
            JsOpResult result;
            result.value = JsValue::make_boolean(op == JsBinaryOp::StrictEq ? equal : !equal);
            return result;
        }
        case JsBinaryOp::Lt:
        case JsBinaryOp::Le:
        case JsBinaryOp::Gt:
        case JsBinaryOp::Ge: {
            if (is_string_type(js_value_type(lhs)) && is_string_type(js_value_type(rhs))) {
                int cmp = 0;
                JsOpError error = JsOpError::None;
                if (!compare_strings(lhs, rhs, cmp, error)) {
                    return make_error(error);
                }
                bool result_value = false;
                if (op == JsBinaryOp::Lt) {
                    result_value = cmp < 0;
                } else if (op == JsBinaryOp::Le) {
                    result_value = cmp <= 0;
                } else if (op == JsBinaryOp::Gt) {
                    result_value = cmp > 0;
                } else {
                    result_value = cmp >= 0;
                }
                JsOpResult result;
                result.value = JsValue::make_boolean(result_value);
                return result;
            }
            if (!is_numeric_like(js_value_type(lhs)) || !is_numeric_like(js_value_type(rhs))) {
                return make_error(JsOpError::TypeError);
            }
            double a = 0.0;
            double b = 0.0;
            if (!to_number(lhs, a) || !to_number(rhs, b)) {
                return make_error(JsOpError::TypeError);
            }
            if (std::isnan(a) || std::isnan(b)) {
                JsOpResult result;
                result.value = JsValue::make_boolean(false);
                return result;
            }
            bool result_value = false;
            if (op == JsBinaryOp::Lt) {
                result_value = a < b;
            } else if (op == JsBinaryOp::Le) {
                result_value = a <= b;
            } else if (op == JsBinaryOp::Gt) {
                result_value = a > b;
            } else {
                result_value = a >= b;
            }
            JsOpResult result;
            result.value = JsValue::make_boolean(result_value);
            return result;
        }
    }
    return make_error(JsOpError::TypeError);
}

} // namespace fiber::json
