//
// Created by dear on 2025/12/30.
//

#include "JsValueOps.h"

#include "Utf.h"

#include <cmath>
#include <cstring>
#include <cstdlib>
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

bool is_string_type(JsNodeType type) {
    return type == JsNodeType::HeapString || type == JsNodeType::NativeString;
}

bool is_number_type(JsNodeType type) {
    return type == JsNodeType::Integer || type == JsNodeType::Float;
}

bool is_numeric_like(JsNodeType type) {
    return type == JsNodeType::Integer || type == JsNodeType::Float || type == JsNodeType::Boolean ||
           type == JsNodeType::Null;
}

bool to_number(const JsValue &value, double &out) {
    switch (value.type_) {
        case JsNodeType::Integer:
            out = static_cast<double>(value.i);
            return true;
        case JsNodeType::Float:
            out = value.f;
            return true;
        case JsNodeType::Boolean:
            out = value.b ? 1.0 : 0.0;
            return true;
        case JsNodeType::Null:
            out = 0.0;
            return true;
        default:
            return false;
    }
}

bool to_int64(const JsValue &value, std::int64_t &out) {
    switch (value.type_) {
        case JsNodeType::Integer:
            out = value.i;
            return true;
        case JsNodeType::Float:
            out = static_cast<std::int64_t>(value.f);
            return true;
        case JsNodeType::Boolean:
            out = value.b ? 1 : 0;
            return true;
        case JsNodeType::Null:
            out = 0;
            return true;
        default:
            return false;
    }
}

bool is_truthy(const JsValue &value) {
    switch (value.type_) {
        case JsNodeType::Undefined:
        case JsNodeType::Null:
            return false;
        case JsNodeType::Boolean:
            return value.b;
        case JsNodeType::Integer:
            return value.i != 0;
        case JsNodeType::Float:
            return value.f != 0.0 && !std::isnan(value.f);
        case JsNodeType::HeapString: {
            auto *str = reinterpret_cast<const GcString *>(value.gc);
            return str && str->len > 0;
        }
        case JsNodeType::NativeString:
            return value.ns.len > 0;
        case JsNodeType::NativeBinary:
            return value.nb.len > 0;
        case JsNodeType::HeapBinary:
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
    if (value.type_ == JsNodeType::HeapString) {
        auto *str = reinterpret_cast<const GcString *>(value.gc);
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
    if (value.type_ == JsNodeType::NativeString) {
        out.kind = StringKind::NativeUtf8;
        out.utf8 = value.ns.data;
        out.len = value.ns.len;
        if (!utf8_scan(out.utf8, out.len, out.scan)) {
            error = JsOpError::InvalidUtf8;
            return false;
        }
        return true;
    }
    error = JsOpError::TypeError;
    return false;
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
        if (out.type_ != JsNodeType::HeapString) {
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
        out.type_ = JsNodeType::HeapString;
        out.gc = &result->hdr;
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
    out.type_ = JsNodeType::HeapString;
    out.gc = &result->hdr;
    return true;
}

bool string_to_utf8_copy(const JsValue &value, std::string &out, JsOpError &error) {
    out.clear();
    if (value.type_ == JsNodeType::NativeString) {
        if (!utf8_validate(value.ns.data, value.ns.len)) {
            error = JsOpError::InvalidUtf8;
            return false;
        }
        if (value.ns.len == 0) {
            return true;
        }
        out.assign(value.ns.data, value.ns.len);
        return true;
    }
    if (value.type_ == JsNodeType::HeapString) {
        auto *str = reinterpret_cast<const GcString *>(value.gc);
        if (!gc_string_to_utf8(str, out)) {
            error = JsOpError::InvalidUtf8;
            return false;
        }
        return true;
    }
    error = JsOpError::TypeError;
    return false;
}

bool ascii_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

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
    if (value.type_ == JsNodeType::HeapString) {
        auto *str = reinterpret_cast<const GcString *>(value.gc);
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
    if (value.type_ == JsNodeType::NativeString) {
        out.kind = StringKind::NativeUtf8;
        out.utf8 = value.ns.data;
        out.len = value.ns.len;
        if (out.len > 0 && !out.utf8) {
            error = JsOpError::InvalidUtf8;
            return false;
        }
        return true;
    }
    error = JsOpError::TypeError;
    return false;
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
    if (lhs.type_ == JsNodeType::HeapString && rhs.type_ == JsNodeType::HeapString) {
        auto *lhs_str = reinterpret_cast<const GcString *>(lhs.gc);
        auto *rhs_str = reinterpret_cast<const GcString *>(rhs.gc);
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
    return value.type_ == JsNodeType::Integer ? static_cast<double>(value.i) : value.f;
}

bool numbers_equal(double lhs, double rhs) {
    if (std::isnan(lhs) || std::isnan(rhs)) {
        return false;
    }
    return lhs == rhs;
}

bool strict_equal(const JsValue &lhs, const JsValue &rhs, JsOpError &error) {
    if (is_string_type(lhs.type_) && is_string_type(rhs.type_)) {
        int cmp = 0;
        if (!compare_strings(lhs, rhs, cmp, error)) {
            return false;
        }
        return cmp == 0;
    }
    if (is_number_type(lhs.type_) && is_number_type(rhs.type_)) {
        return numbers_equal(number_value(lhs), number_value(rhs));
    }
    if (lhs.type_ != rhs.type_) {
        return false;
    }
    switch (lhs.type_) {
        case JsNodeType::Undefined:
        case JsNodeType::Null:
            return true;
        case JsNodeType::Boolean:
            return lhs.b == rhs.b;
        case JsNodeType::Integer:
            return lhs.i == rhs.i;
        case JsNodeType::Float:
            return lhs.f == rhs.f;
        case JsNodeType::NativeBinary:
            return lhs.nb.data == rhs.nb.data && lhs.nb.len == rhs.nb.len;
        case JsNodeType::HeapBinary:
        case JsNodeType::Array:
        case JsNodeType::Object:
        case JsNodeType::Interator:
        case JsNodeType::Exception:
            return lhs.gc == rhs.gc;
        case JsNodeType::HeapString:
        case JsNodeType::NativeString:
            break;
    }
    return false;
}

bool loose_equal_number(double number, const JsValue &other, JsOpError &error) {
    if (is_number_type(other.type_)) {
        return numbers_equal(number, number_value(other));
    }
    if (is_string_type(other.type_)) {
        double other_number = 0.0;
        if (!string_to_number(other, other_number, error)) {
            return false;
        }
        return numbers_equal(number, other_number);
    }
    return false;
}

bool loose_equal(const JsValue &lhs, const JsValue &rhs, JsOpError &error) {
    if (is_string_type(lhs.type_) && is_string_type(rhs.type_)) {
        int cmp = 0;
        if (!compare_strings(lhs, rhs, cmp, error)) {
            return false;
        }
        return cmp == 0;
    }
    if (is_number_type(lhs.type_) && is_number_type(rhs.type_)) {
        return numbers_equal(number_value(lhs), number_value(rhs));
    }
    if (lhs.type_ == rhs.type_) {
        return strict_equal(lhs, rhs, error);
    }
    if ((lhs.type_ == JsNodeType::Null && rhs.type_ == JsNodeType::Undefined) ||
        (lhs.type_ == JsNodeType::Undefined && rhs.type_ == JsNodeType::Null)) {
        return true;
    }
    if (lhs.type_ == JsNodeType::Boolean) {
        double lhs_number = lhs.b ? 1.0 : 0.0;
        return loose_equal_number(lhs_number, rhs, error);
    }
    if (rhs.type_ == JsNodeType::Boolean) {
        double rhs_number = rhs.b ? 1.0 : 0.0;
        return loose_equal_number(rhs_number, lhs, error);
    }
    if (is_number_type(lhs.type_) && is_string_type(rhs.type_)) {
        double rhs_number = 0.0;
        if (!string_to_number(rhs, rhs_number, error)) {
            return false;
        }
        return numbers_equal(number_value(lhs), rhs_number);
    }
    if (is_string_type(lhs.type_) && is_number_type(rhs.type_)) {
        double lhs_number = 0.0;
        if (!string_to_number(lhs, lhs_number, error)) {
            return false;
        }
        return numbers_equal(lhs_number, number_value(rhs));
    }
    return false;
}

JsOpResult add_numeric(const JsValue &lhs, const JsValue &rhs) {
    if (!is_numeric_like(lhs.type_) || !is_numeric_like(rhs.type_)) {
        return make_error(JsOpError::TypeError);
    }
    if (lhs.type_ == JsNodeType::Float || rhs.type_ == JsNodeType::Float) {
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
    if (!is_numeric_like(lhs.type_) || !is_numeric_like(rhs.type_)) {
        return make_error(JsOpError::TypeError);
    }
    if (lhs.type_ == JsNodeType::Float || rhs.type_ == JsNodeType::Float) {
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
    if (!is_numeric_like(lhs.type_) || !is_numeric_like(rhs.type_)) {
        return make_error(JsOpError::TypeError);
    }
    if (lhs.type_ == JsNodeType::Float || rhs.type_ == JsNodeType::Float) {
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
            if (!is_numeric_like(value.type_)) {
                return make_error(JsOpError::TypeError);
            }
            if (value.type_ == JsNodeType::Float) {
                JsOpResult result;
                result.value = JsValue::make_float(value.f);
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
            if (!is_numeric_like(value.type_)) {
                return make_error(JsOpError::TypeError);
            }
            if (value.type_ == JsNodeType::Float) {
                JsOpResult result;
                result.value = JsValue::make_float(-value.f);
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
            if (is_string_type(lhs.type_) || is_string_type(rhs.type_)) {
                if (!is_string_type(lhs.type_) || !is_string_type(rhs.type_)) {
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
            if (!is_numeric_like(lhs.type_) || !is_numeric_like(rhs.type_)) {
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
            if (!is_numeric_like(lhs.type_) || !is_numeric_like(rhs.type_)) {
                return make_error(JsOpError::TypeError);
            }
            if (lhs.type_ == JsNodeType::Float || rhs.type_ == JsNodeType::Float) {
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
            if (is_string_type(lhs.type_) && is_string_type(rhs.type_)) {
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
            if (!is_numeric_like(lhs.type_) || !is_numeric_like(rhs.type_)) {
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
