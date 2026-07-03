#include "Compares.h"

#include "../../common/json/Utf.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

namespace fiber::script::run {

namespace {

bool is_string_type(fiber::json::JsNodeType type) noexcept { return type == fiber::json::JsNodeType::String; }

bool is_number_type(fiber::json::JsNodeType type) noexcept {
    return type == fiber::json::JsNodeType::Integer || type == fiber::json::JsNodeType::Float;
}

bool is_numeric_like(fiber::json::JsNodeType type) noexcept {
    return type == fiber::json::JsNodeType::Integer || type == fiber::json::JsNodeType::Float ||
           type == fiber::json::JsNodeType::Boolean || type == fiber::json::JsNodeType::Null;
}

const fiber::json::GcString *as_heap_string(const fiber::json::JsValue &value) noexcept {
    return fiber::json::js_value_type(value) == fiber::json::JsNodeType::String
                   ? fiber::json::js_value_heap_ptr<const fiber::json::GcString>(value)
                   : nullptr;
}

bool to_number(const fiber::json::JsValue &value, double &out) noexcept {
    switch (fiber::json::js_value_type(value)) {
        case fiber::json::JsNodeType::Integer:
            out = static_cast<double>(fiber::json::js_value_int64(value));
            return true;
        case fiber::json::JsNodeType::Float:
            out = fiber::json::js_value_double(value);
            return true;
        case fiber::json::JsNodeType::Boolean:
            out = fiber::json::js_value_bool(value) ? 1.0 : 0.0;
            return true;
        case fiber::json::JsNodeType::Null:
            out = 0.0;
            return true;
        default:
            return false;
    }
}

bool is_truthy(const fiber::json::JsValue &value) noexcept {
    switch (fiber::json::js_value_type(value)) {
        case fiber::json::JsNodeType::Undefined:
        case fiber::json::JsNodeType::Null:
            return false;
        case fiber::json::JsNodeType::Boolean:
            return fiber::json::js_value_bool(value);
        case fiber::json::JsNodeType::Integer:
            return fiber::json::js_value_int64(value) != 0;
        case fiber::json::JsNodeType::Float:
            return fiber::json::js_value_double(value) != 0.0 && !std::isnan(fiber::json::js_value_double(value));
        case fiber::json::JsNodeType::String:
            if (fiber::json::js_value_is_borrowed_string(value)) {
                return fiber::json::js_value_native_string(value).len > 0;
            }
            if (auto *str = as_heap_string(value)) {
                return str->len > 0;
            }
            return false;
        case fiber::json::JsNodeType::Binary:
        case fiber::json::JsNodeType::Array:
        case fiber::json::JsNodeType::Object:
        case fiber::json::JsNodeType::Interator:
        case fiber::json::JsNodeType::Exception:
            return true;
    }
    return false;
}

ScriptStatus make_bool(ValueHandle out, bool value) noexcept {
    if (!out) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
    }
    *out = fiber::json::JsValue::make_boolean(value);
    return ScriptStatus::success();
}

enum class StringKind : std::uint8_t {
    HeapByte,
    HeapUtf16,
    NativeUtf8,
};

bool string_to_utf8_copy(const fiber::json::JsValue &value, std::string &out, ScriptAbortReason &error) {
    out.clear();
    if (fiber::json::js_value_type(value) != fiber::json::JsNodeType::String) {
        error = ScriptAbortReason::TypeError;
        return false;
    }
    if (fiber::json::js_value_is_borrowed_string(value)) {
        fiber::json::NativeStr native = fiber::json::js_value_native_string(value);
        if (!fiber::json::utf8_validate(native.data, native.len)) {
            error = ScriptAbortReason::InvalidArgument;
            return false;
        }
        if (native.len > 0) {
            out.assign(native.data, native.len);
        }
        return true;
    }
    auto *str = as_heap_string(value);
    if (!fiber::json::gc_string_to_utf8(str, out)) {
        error = ScriptAbortReason::InvalidArgument;
        return false;
    }
    return true;
}

bool ascii_is_space(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

bool string_to_number(const fiber::json::JsValue &value, double &out, ScriptAbortReason &error) {
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

bool init_string_cursor(const fiber::json::JsValue &value, StringCursor &out, ScriptAbortReason &error) {
    if (fiber::json::js_value_type(value) != fiber::json::JsNodeType::String) {
        error = ScriptAbortReason::TypeError;
        return false;
    }
    if (!fiber::json::js_value_is_borrowed_string(value)) {
        auto *str = as_heap_string(value);
        if (!str) {
            error = ScriptAbortReason::TypeError;
            return false;
        }
        if (str->encoding == fiber::json::GcStringEncoding::Byte) {
            out.kind = StringKind::HeapByte;
            out.bytes = str->data8;
        } else {
            out.kind = StringKind::HeapUtf16;
            out.u16 = str->data16;
        }
        out.len = str->len;
        if (out.len > 0 && !out.bytes && !out.u16) {
            error = ScriptAbortReason::TypeError;
            return false;
        }
        return true;
    }
    out.kind = StringKind::NativeUtf8;
    out.utf8 = fiber::json::js_value_native_string(value).data;
    out.len = fiber::json::js_value_native_string(value).len;
    if (out.len > 0 && !out.utf8) {
        error = ScriptAbortReason::InvalidArgument;
        return false;
    }
    return true;
}

bool cursor_next(StringCursor &cursor, char16_t &unit, ScriptAbortReason &error) {
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
            if (!fiber::json::utf8_next_codepoint(cursor.utf8, cursor.len, cursor.pos, codepoint)) {
                error = ScriptAbortReason::InvalidArgument;
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
    error = ScriptAbortReason::TypeError;
    return false;
}

bool compare_strings(const fiber::json::JsValue &lhs, const fiber::json::JsValue &rhs, int &result,
                     ScriptAbortReason &error) {
    if (!fiber::json::js_value_is_borrowed_string(lhs) && !fiber::json::js_value_is_borrowed_string(rhs) &&
        fiber::json::js_value_type(lhs) == fiber::json::JsNodeType::String &&
        fiber::json::js_value_type(rhs) == fiber::json::JsNodeType::String) {
        auto *lhs_str = as_heap_string(lhs);
        auto *rhs_str = as_heap_string(rhs);
        if (!lhs_str || !rhs_str) {
            error = ScriptAbortReason::TypeError;
            return false;
        }
        if (lhs_str->encoding == fiber::json::GcStringEncoding::Byte &&
            rhs_str->encoding == fiber::json::GcStringEncoding::Byte) {
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
        if (lhs_str->encoding == fiber::json::GcStringEncoding::Utf16 &&
            rhs_str->encoding == fiber::json::GcStringEncoding::Utf16) {
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
    if (!init_string_cursor(lhs, lhs_cursor, error) || !init_string_cursor(rhs, rhs_cursor, error)) {
        return false;
    }
    while (true) {
        char16_t lhs_unit = 0;
        char16_t rhs_unit = 0;
        bool lhs_has = cursor_next(lhs_cursor, lhs_unit, error);
        if (error != ScriptAbortReason::None) {
            return false;
        }
        bool rhs_has = cursor_next(rhs_cursor, rhs_unit, error);
        if (error != ScriptAbortReason::None) {
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

double number_value(const fiber::json::JsValue &value) noexcept {
    return fiber::json::js_value_type(value) == fiber::json::JsNodeType::Integer
                   ? static_cast<double>(fiber::json::js_value_int64(value))
                   : fiber::json::js_value_double(value);
}

bool numbers_equal(double lhs, double rhs) noexcept {
    if (std::isnan(lhs) || std::isnan(rhs)) {
        return false;
    }
    return lhs == rhs;
}

bool strict_equal(const fiber::json::JsValue &lhs, const fiber::json::JsValue &rhs, ScriptAbortReason &error) {
    if (is_string_type(fiber::json::js_value_type(lhs)) && is_string_type(fiber::json::js_value_type(rhs))) {
        int cmp = 0;
        if (!compare_strings(lhs, rhs, cmp, error)) {
            return false;
        }
        return cmp == 0;
    }
    if (is_number_type(fiber::json::js_value_type(lhs)) && is_number_type(fiber::json::js_value_type(rhs))) {
        return numbers_equal(number_value(lhs), number_value(rhs));
    }
    if (fiber::json::js_value_type(lhs) != fiber::json::js_value_type(rhs)) {
        return false;
    }
    switch (fiber::json::js_value_type(lhs)) {
        case fiber::json::JsNodeType::Undefined:
        case fiber::json::JsNodeType::Null:
            return true;
        case fiber::json::JsNodeType::Boolean:
            return fiber::json::js_value_bool(lhs) == fiber::json::js_value_bool(rhs);
        case fiber::json::JsNodeType::Integer:
            return fiber::json::js_value_int64(lhs) == fiber::json::js_value_int64(rhs);
        case fiber::json::JsNodeType::Float:
            return fiber::json::js_value_double(lhs) == fiber::json::js_value_double(rhs);
        case fiber::json::JsNodeType::Binary:
            if (fiber::json::js_value_is_borrowed_binary(lhs) || fiber::json::js_value_is_borrowed_binary(rhs)) {
                fiber::json::NativeBin lhs_bin = fiber::json::js_value_native_binary(lhs);
                fiber::json::NativeBin rhs_bin = fiber::json::js_value_native_binary(rhs);
                return lhs_bin.data == rhs_bin.data && lhs_bin.len == rhs_bin.len;
            }
            return fiber::json::js_value_heap_header(lhs) == fiber::json::js_value_heap_header(rhs);
        case fiber::json::JsNodeType::Array:
        case fiber::json::JsNodeType::Object:
        case fiber::json::JsNodeType::Interator:
        case fiber::json::JsNodeType::Exception:
            return fiber::json::js_value_heap_header(lhs) == fiber::json::js_value_heap_header(rhs);
        case fiber::json::JsNodeType::String:
            break;
    }
    return false;
}

bool loose_equal_number(double number, const fiber::json::JsValue &other, ScriptAbortReason &error) {
    if (is_number_type(fiber::json::js_value_type(other))) {
        return numbers_equal(number, number_value(other));
    }
    if (is_string_type(fiber::json::js_value_type(other))) {
        double other_number = 0.0;
        if (!string_to_number(other, other_number, error)) {
            return false;
        }
        return numbers_equal(number, other_number);
    }
    return false;
}

bool loose_equal(const fiber::json::JsValue &lhs, const fiber::json::JsValue &rhs, ScriptAbortReason &error) {
    if (is_string_type(fiber::json::js_value_type(lhs)) && is_string_type(fiber::json::js_value_type(rhs))) {
        int cmp = 0;
        if (!compare_strings(lhs, rhs, cmp, error)) {
            return false;
        }
        return cmp == 0;
    }
    if (is_number_type(fiber::json::js_value_type(lhs)) && is_number_type(fiber::json::js_value_type(rhs))) {
        return numbers_equal(number_value(lhs), number_value(rhs));
    }
    if (fiber::json::js_value_type(lhs) == fiber::json::js_value_type(rhs)) {
        return strict_equal(lhs, rhs, error);
    }
    if ((fiber::json::js_value_type(lhs) == fiber::json::JsNodeType::Null &&
         fiber::json::js_value_type(rhs) == fiber::json::JsNodeType::Undefined) ||
        (fiber::json::js_value_type(lhs) == fiber::json::JsNodeType::Undefined &&
         fiber::json::js_value_type(rhs) == fiber::json::JsNodeType::Null)) {
        return true;
    }
    if (fiber::json::js_value_type(lhs) == fiber::json::JsNodeType::Boolean) {
        double lhs_number = fiber::json::js_value_bool(lhs) ? 1.0 : 0.0;
        return loose_equal_number(lhs_number, rhs, error);
    }
    if (fiber::json::js_value_type(rhs) == fiber::json::JsNodeType::Boolean) {
        double rhs_number = fiber::json::js_value_bool(rhs) ? 1.0 : 0.0;
        return loose_equal_number(rhs_number, lhs, error);
    }
    if (is_number_type(fiber::json::js_value_type(lhs)) && is_string_type(fiber::json::js_value_type(rhs))) {
        double rhs_number = 0.0;
        if (!string_to_number(rhs, rhs_number, error)) {
            return false;
        }
        return numbers_equal(number_value(lhs), rhs_number);
    }
    if (is_string_type(fiber::json::js_value_type(lhs)) && is_number_type(fiber::json::js_value_type(rhs))) {
        double lhs_number = 0.0;
        if (!string_to_number(lhs, lhs_number, error)) {
            return false;
        }
        return numbers_equal(lhs_number, number_value(rhs));
    }
    return false;
}

ScriptStatus equality(ValueHandle out, ConstValueHandle a, ConstValueHandle b, bool strict, bool invert) {
    if (!out) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
    }
    ScriptAbortReason error = ScriptAbortReason::None;
    bool equal = strict ? strict_equal(*a, *b, error) : loose_equal(*a, *b, error);
    if (error != ScriptAbortReason::None) {
        return ScriptStatus::abort(error);
    }
    *out = fiber::json::JsValue::make_boolean(invert ? !equal : equal);
    return ScriptStatus::success();
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

ScriptStatus relation(ValueHandle out, ConstValueHandle a, ConstValueHandle b, bool less_direction, bool allow_equal) {
    if (!out) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
    }
    if (is_string_type(fiber::json::js_value_type(*a)) && is_string_type(fiber::json::js_value_type(*b))) {
        int cmp = 0;
        ScriptAbortReason error = ScriptAbortReason::None;
        if (!compare_strings(*a, *b, cmp, error)) {
            return ScriptStatus::abort(error);
        }
        *out = fiber::json::JsValue::make_boolean(relation_result(cmp, less_direction, allow_equal));
        return ScriptStatus::success();
    }
    if (!is_numeric_like(fiber::json::js_value_type(*a)) || !is_numeric_like(fiber::json::js_value_type(*b))) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    double lhs = 0.0;
    double rhs = 0.0;
    if (!to_number(*a, lhs) || !to_number(*b, rhs)) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    *out = fiber::json::JsValue::make_boolean(relation_result(lhs, rhs, less_direction, allow_equal));
    return ScriptStatus::success();
}

} // namespace

bool Compares::neg(ConstValueHandle value) noexcept { return !logic(value); }

bool Compares::logic(ConstValueHandle value) noexcept {
    if (!value) {
        return false;
    }
    return is_truthy(*value);
}

ScriptStatus Compares::eq(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return equality(out, a, b, false, false);
}

ScriptStatus Compares::seq(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return equality(out, a, b, true, false);
}

ScriptStatus Compares::ne(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return equality(out, a, b, false, true);
}

ScriptStatus Compares::sne(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return equality(out, a, b, true, true);
}

ScriptStatus Compares::lt(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return relation(out, a, b, true, false);
}

ScriptStatus Compares::lte(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return relation(out, a, b, true, true);
}

ScriptStatus Compares::gt(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return relation(out, a, b, false, false);
}

ScriptStatus Compares::gte(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return relation(out, a, b, false, true);
}

ScriptStatus Compares::matches(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    (void) a;
    (void) b;
    return make_bool(out, false);
}

ScriptStatus Compares::in(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    if (!a || !b) {
        return make_bool(out, false);
    }
    if (fiber::json::js_value_type(*b) == fiber::json::JsNodeType::Array) {
        if (fiber::json::js_value_type(*a) != fiber::json::JsNodeType::Integer) {
            return make_bool(out, false);
        }
        auto *arr = fiber::json::js_value_heap_ptr<const fiber::json::GcArray>(*b);
        std::int64_t index = fiber::json::js_value_int64(*a);
        if (!arr || index < 0) {
            return make_bool(out, false);
        }
        return make_bool(out, static_cast<std::size_t>(index) < arr->size);
    }
    if (fiber::json::js_value_type(*b) == fiber::json::JsNodeType::Object) {
        auto *obj = fiber::json::js_value_heap_ptr<const fiber::json::GcObject>(*b);
        if (!obj) {
            return make_bool(out, false);
        }
        if (fiber::json::js_value_type(*a) == fiber::json::JsNodeType::String &&
            !fiber::json::js_value_is_borrowed_string(*a)) {
            auto *key_str = fiber::json::js_value_heap_ptr<const fiber::json::GcString>(*a);
            const fiber::json::JsValue *found = fiber::json::gc_object_get(obj, key_str);
            return make_bool(out, found != nullptr);
        }
        if (fiber::json::js_value_type(*a) == fiber::json::JsNodeType::String &&
            fiber::json::js_value_is_borrowed_string(*a)) {
            fiber::json::NativeStr native = fiber::json::js_value_native_string(*a);
            std::string key(native.data, native.len);
            for (std::size_t i = 0; i < obj->size; ++i) {
                const fiber::json::GcObjectEntry *entry = fiber::json::gc_object_entry_at(obj, i);
                if (!entry || !entry->occupied || !entry->key) {
                    continue;
                }
                std::string entry_key;
                if (fiber::json::gc_string_to_utf8(entry->key, entry_key) && entry_key == key) {
                    return make_bool(out, true);
                }
            }
        }
        return make_bool(out, false);
    }
    return make_bool(out, false);
}

} // namespace fiber::script::run
