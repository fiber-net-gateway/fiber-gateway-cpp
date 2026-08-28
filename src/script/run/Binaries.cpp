#include "script/run/Binaries.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include <fiber/common/json/Utf.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/run/Compares.h>
#include "script/gc/Wtf8.h"

namespace fiber::script::run {

namespace {

bool is_string_like(const fiber::script::JsValue &value) noexcept {
    return fiber::script::js_value_type(value) == fiber::script::JsNodeType::String;
}

bool is_numeric_like(fiber::script::JsNodeType type) noexcept {
    return type == fiber::script::JsNodeType::Integer || type == fiber::script::JsNodeType::Float ||
           type == fiber::script::JsNodeType::Boolean || type == fiber::script::JsNodeType::Null;
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

bool to_int64(const fiber::script::JsValue &value, std::int64_t &out) noexcept {
    switch (fiber::script::js_value_type(value)) {
        case fiber::script::JsNodeType::Integer:
            out = fiber::script::js_value_int64(value);
            return true;
        case fiber::script::JsNodeType::Float:
            out = static_cast<std::int64_t>(fiber::script::js_value_double(value));
            return true;
        case fiber::script::JsNodeType::Boolean:
            out = fiber::script::js_value_bool(value) ? 1 : 0;
            return true;
        case fiber::script::JsNodeType::Null:
            out = 0;
            return true;
        default:
            return false;
    }
}

const fiber::script::GcString *as_heap_string(const fiber::script::JsValue &value) noexcept {
    return fiber::script::js_value_type(value) == fiber::script::JsNodeType::String
                   ? fiber::script::js_value_heap_ptr<const fiber::script::GcString>(value)
                   : nullptr;
}

struct StringSource {
    const char *data = nullptr;
    std::size_t byte_len = 0;
    std::size_t utf16_len = 0;
    bool well_formed = true;
};

void set_ascii_string_source(StringSource &out, const char *data, std::size_t len) noexcept {
    out.data = data;
    out.byte_len = len;
    out.utf16_len = len;
    out.well_formed = true;
}

// Returns false on any non-string-coercible input; callers raise TypeError. Malformed UTF-8 in a
// borrowed string also fails (coerces to TypeError), matching the operator exception policy.
bool build_string_source(const fiber::script::JsValue &value, StringSource &out) noexcept {
    if (fiber::script::js_value_type(value) != fiber::script::JsNodeType::String) {
        return false;
    }
    if (!fiber::script::js_value_is_borrowed_string(value)) {
        auto *str = as_heap_string(value);
        if (!str) {
            return false;
        }
        out.data = fiber::script::gc_string_wtf8_data(str);
        out.byte_len = fiber::script::gc_string_byte_len(str);
        out.utf16_len = str->utf16_len;
        out.well_formed = fiber::script::gc_string_is_well_formed(str);
        return true;
    }
    fiber::script::NativeStr native = fiber::script::js_value_native_string(value);
    fiber::json::Utf8ScanResult scan;
    if (!fiber::json::utf8_scan(native.data, native.len, scan)) {
        return false;
    }
    out.data = native.data;
    out.byte_len = native.len;
    out.utf16_len = scan.utf16_len;
    out.well_formed = true;
    return true;
}

bool primitive_to_string_source(const fiber::script::JsValue &value, StringSource &out, char *buffer,
                                std::size_t buffer_len) {
    if (fiber::script::js_value_type(value) == fiber::script::JsNodeType::String) {
        return build_string_source(value, out);
    }
    switch (fiber::script::js_value_type(value)) {
        case fiber::script::JsNodeType::Undefined:
            set_ascii_string_source(out, "undefined", 9);
            return true;
        case fiber::script::JsNodeType::Null:
            set_ascii_string_source(out, "null", 4);
            return true;
        case fiber::script::JsNodeType::Boolean:
            if (fiber::script::js_value_bool(value)) {
                set_ascii_string_source(out, "true", 4);
            } else {
                set_ascii_string_source(out, "false", 5);
            }
            return true;
        case fiber::script::JsNodeType::Integer: {
            auto converted = std::to_chars(buffer, buffer + buffer_len, fiber::script::js_value_int64(value));
            if (converted.ec != std::errc{}) {
                return false;
            }
            set_ascii_string_source(out, buffer, static_cast<std::size_t>(converted.ptr - buffer));
            return true;
        }
        case fiber::script::JsNodeType::Float: {
            double number = fiber::script::js_value_double(value);
            if (std::isnan(number)) {
                set_ascii_string_source(out, "NaN", 3);
                return true;
            }
            if (std::isinf(number)) {
                if (number < 0) {
                    set_ascii_string_source(out, "-Infinity", 9);
                } else {
                    set_ascii_string_source(out, "Infinity", 8);
                }
                return true;
            }
            auto converted = std::to_chars(buffer, buffer + buffer_len, number);
            if (converted.ec != std::errc{}) {
                return false;
            }
            set_ascii_string_source(out, buffer, static_cast<std::size_t>(converted.ptr - buffer));
            return true;
        }
        case fiber::script::JsNodeType::String:
            return build_string_source(value, out);
        case fiber::script::JsNodeType::Array:
        case fiber::script::JsNodeType::Object:
        case fiber::script::JsNodeType::Interator:
        case fiber::script::JsNodeType::Exception:
        case fiber::script::JsNodeType::Binary:
            return false;
    }
    return false;
}

CallResult concat_strings(fiber::script::GcHeap &heap, const StringSource &lhs, const StringSource &rhs,
                          ResultPayload &result) {
    if (lhs.utf16_len > std::numeric_limits<std::uint32_t>::max() - rhs.utf16_len ||
        lhs.byte_len > std::numeric_limits<std::size_t>::max() - rhs.byte_len) {
        return set_abort(result, ScriptAbortReason::OutOfMemory);
    }
    const std::size_t utf16_len = lhs.utf16_len + rhs.utf16_len;
    std::size_t byte_len = lhs.byte_len + rhs.byte_len;

    char16_t high = 0;
    char16_t low = 0;
    const bool merge_boundary = fiber::script::gc_detail::wtf8_ends_with_high_surrogate(lhs.data, lhs.byte_len, high) &&
                                fiber::script::gc_detail::wtf8_starts_with_low_surrogate(rhs.data, rhs.byte_len, low);
    if (merge_boundary) {
        byte_len -= 2;
    }

    fiber::script::GcHeap::LocalMark mark(heap);
    fiber::script::ValueHandle out = heap.local_value();
    if (!out) {
        return set_abort(result, ScriptAbortReason::OutOfMemory);
    }

    if (utf16_len == 0) {
        if (!fiber::script::gc_make_string(&heap, out, "", 0)) {
            return set_abort(result, ScriptAbortReason::OutOfMemory);
        }
        return set_value(result, *out);
    }

    fiber::script::GcHeap::NoGcScope no_gc(heap);
    const bool initially_well_formed = lhs.well_formed && rhs.well_formed;
    fiber::script::GcString *str =
            fiber::script::gc_new_string_wtf8_uninit(&heap, byte_len, utf16_len, initially_well_formed);
    if (!str) {
        return set_abort(result, ScriptAbortReason::OutOfMemory);
    }
    char *dst = fiber::script::gc_string_wtf8_data(str);
    std::size_t offset = 0;
    if (!merge_boundary) {
        if (lhs.byte_len > 0) {
            std::memcpy(dst, lhs.data, lhs.byte_len);
        }
        if (rhs.byte_len > 0) {
            std::memcpy(dst + lhs.byte_len, rhs.data, rhs.byte_len);
        }
    } else {
        const std::size_t lhs_copy = lhs.byte_len - 3;
        if (lhs_copy > 0) {
            std::memcpy(dst, lhs.data, lhs_copy);
        }
        offset = lhs_copy;
        const std::uint32_t codepoint = 0x10000 + ((static_cast<std::uint32_t>(high) - 0xD800) << 10) +
                                        (static_cast<std::uint32_t>(low) - 0xDC00);
        offset += fiber::script::gc_detail::wtf8_write_codepoint(codepoint, dst + offset);
        const std::size_t rhs_copy = rhs.byte_len - 3;
        if (rhs_copy > 0) {
            std::memcpy(dst + offset, rhs.data + 3, rhs_copy);
            offset += rhs_copy;
        }
        if (!initially_well_formed && fiber::script::gc_detail::wtf8_is_well_formed(dst, byte_len)) {
            str->flags |= fiber::script::kGcStringWellFormed;
        }
    }
    fiber::script::GcString *interned = fiber::script::gc_detail::gc_string_intern_final(&heap, str);
    *out = fiber::script::js_make_heap_ref(&interned->hdr, fiber::script::GcHeapKind::String);
    return set_value(result, *out);
}

CallResult add_numeric(const fiber::script::JsValue &lhs, const fiber::script::JsValue &rhs,
                       ResultPayload &result) noexcept {
    if (!is_numeric_like(fiber::script::js_value_type(lhs)) || !is_numeric_like(fiber::script::js_value_type(rhs))) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (fiber::script::js_value_type(lhs) == fiber::script::JsNodeType::Float ||
        fiber::script::js_value_type(rhs) == fiber::script::JsNodeType::Float) {
        double a = 0.0;
        double b = 0.0;
        if (!to_number(lhs, a) || !to_number(rhs, b)) {
            return set_exception(result, fiber::script::ExceptionKind::TypeError);
        }
        return set_value(result, fiber::script::JsValue::make_float(a + b));
    }
    std::int64_t a = 0;
    std::int64_t b = 0;
    if (!to_int64(lhs, a) || !to_int64(rhs, b)) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    std::int64_t sum = 0;
    if (!__builtin_add_overflow(a, b, &sum)) {
        return set_value(result, fiber::script::JsValue::make_integer(sum));
    }
    return set_value(result, fiber::script::JsValue::make_float(static_cast<double>(a) + static_cast<double>(b)));
}

CallResult sub_numeric(const fiber::script::JsValue &lhs, const fiber::script::JsValue &rhs,
                       ResultPayload &result) noexcept {
    if (!is_numeric_like(fiber::script::js_value_type(lhs)) || !is_numeric_like(fiber::script::js_value_type(rhs))) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (fiber::script::js_value_type(lhs) == fiber::script::JsNodeType::Float ||
        fiber::script::js_value_type(rhs) == fiber::script::JsNodeType::Float) {
        double a = 0.0;
        double b = 0.0;
        if (!to_number(lhs, a) || !to_number(rhs, b)) {
            return set_exception(result, fiber::script::ExceptionKind::TypeError);
        }
        return set_value(result, fiber::script::JsValue::make_float(a - b));
    }
    std::int64_t a = 0;
    std::int64_t b = 0;
    if (!to_int64(lhs, a) || !to_int64(rhs, b)) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    std::int64_t diff = 0;
    if (!__builtin_sub_overflow(a, b, &diff)) {
        return set_value(result, fiber::script::JsValue::make_integer(diff));
    }
    return set_value(result, fiber::script::JsValue::make_float(static_cast<double>(a) - static_cast<double>(b)));
}

CallResult mul_numeric(const fiber::script::JsValue &lhs, const fiber::script::JsValue &rhs,
                       ResultPayload &result) noexcept {
    if (!is_numeric_like(fiber::script::js_value_type(lhs)) || !is_numeric_like(fiber::script::js_value_type(rhs))) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (fiber::script::js_value_type(lhs) == fiber::script::JsNodeType::Float ||
        fiber::script::js_value_type(rhs) == fiber::script::JsNodeType::Float) {
        double a = 0.0;
        double b = 0.0;
        if (!to_number(lhs, a) || !to_number(rhs, b)) {
            return set_exception(result, fiber::script::ExceptionKind::TypeError);
        }
        return set_value(result, fiber::script::JsValue::make_float(a * b));
    }
    std::int64_t a = 0;
    std::int64_t b = 0;
    if (!to_int64(lhs, a) || !to_int64(rhs, b)) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    std::int64_t product = 0;
    if (!__builtin_mul_overflow(a, b, &product)) {
        return set_value(result, fiber::script::JsValue::make_integer(product));
    }
    return set_value(result, fiber::script::JsValue::make_float(static_cast<double>(a) * static_cast<double>(b)));
}

CallResult plus_impl(fiber::script::GcHeap &heap, const fiber::script::JsValue &lhs, const fiber::script::JsValue &rhs,
                     ResultPayload &result) {
    if (is_string_like(lhs) || is_string_like(rhs)) {
        StringSource lhs_src;
        StringSource rhs_src;
        char lhs_buf[64];
        char rhs_buf[64];
        if (!primitive_to_string_source(lhs, lhs_src, lhs_buf, sizeof(lhs_buf)) ||
            !primitive_to_string_source(rhs, rhs_src, rhs_buf, sizeof(rhs_buf))) {
            return set_exception(result, fiber::script::ExceptionKind::TypeError);
        }
        return concat_strings(heap, lhs_src, rhs_src, result);
    }
    return add_numeric(lhs, rhs, result);
}

} // namespace

CallResult Binaries::plus(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    return plus_impl(runtime.heap(), *a, *b, result);
}

CallResult Binaries::minus(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    (void) runtime;
    return sub_numeric(*a, *b, result);
}

CallResult Binaries::multiply(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    (void) runtime;
    return mul_numeric(*a, *b, result);
}

CallResult Binaries::divide(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    (void) runtime;
    if (!is_numeric_like(fiber::script::js_value_type(*a)) || !is_numeric_like(fiber::script::js_value_type(*b))) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    double lhs = 0.0;
    double rhs = 0.0;
    if (!to_number(*a, lhs) || !to_number(*b, rhs)) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (rhs == 0.0) {
        return set_exception(result, fiber::script::ExceptionKind::RangeError);
    }
    return set_value(result, fiber::script::JsValue::make_float(lhs / rhs));
}

CallResult Binaries::modulo(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    (void) runtime;
    if (!is_numeric_like(fiber::script::js_value_type(*a)) || !is_numeric_like(fiber::script::js_value_type(*b))) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (fiber::script::js_value_type(*a) == fiber::script::JsNodeType::Float ||
        fiber::script::js_value_type(*b) == fiber::script::JsNodeType::Float) {
        double lhs = 0.0;
        double rhs = 0.0;
        if (!to_number(*a, lhs) || !to_number(*b, rhs)) {
            return set_exception(result, fiber::script::ExceptionKind::TypeError);
        }
        if (rhs == 0.0) {
            return set_exception(result, fiber::script::ExceptionKind::RangeError);
        }
        return set_value(result, fiber::script::JsValue::make_float(std::fmod(lhs, rhs)));
    }
    std::int64_t lhs = 0;
    std::int64_t rhs = 0;
    if (!to_int64(*a, lhs) || !to_int64(*b, rhs)) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (rhs == 0) {
        return set_exception(result, fiber::script::ExceptionKind::RangeError);
    }
    return set_value(result, fiber::script::JsValue::make_integer(lhs % rhs));
}

CallResult Binaries::matches(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    (void) runtime;
    return set_value(result, fiber::script::JsValue::make_boolean(Compares::matches(a, b)));
}

CallResult Binaries::lt(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    (void) runtime;
    return set_value(result, fiber::script::JsValue::make_boolean(Compares::lt(a, b)));
}

CallResult Binaries::lte(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    (void) runtime;
    return set_value(result, fiber::script::JsValue::make_boolean(Compares::lte(a, b)));
}

CallResult Binaries::gt(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    (void) runtime;
    return set_value(result, fiber::script::JsValue::make_boolean(Compares::gt(a, b)));
}

CallResult Binaries::gte(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    (void) runtime;
    return set_value(result, fiber::script::JsValue::make_boolean(Compares::gte(a, b)));
}

CallResult Binaries::eq(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    (void) runtime;
    return set_value(result, fiber::script::JsValue::make_boolean(Compares::eq(a, b)));
}

CallResult Binaries::seq(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    (void) runtime;
    return set_value(result, fiber::script::JsValue::make_boolean(Compares::seq(a, b)));
}

CallResult Binaries::ne(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    (void) runtime;
    return set_value(result, fiber::script::JsValue::make_boolean(Compares::ne(a, b)));
}

CallResult Binaries::sne(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    (void) runtime;
    return set_value(result, fiber::script::JsValue::make_boolean(Compares::sne(a, b)));
}

CallResult Binaries::in(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept {
    (void) runtime;
    return set_value(result, fiber::script::JsValue::make_boolean(Compares::in(a, b)));
}

} // namespace fiber::script::run
