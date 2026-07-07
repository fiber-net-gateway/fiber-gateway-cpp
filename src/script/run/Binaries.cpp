#include "Binaries.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "../../common/json/Utf.h"
#include "../JsGc.h"
#include "Compares.h"

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

std::size_t string_code_unit_upper_bound(const fiber::script::JsValue &value) noexcept {
    if (fiber::script::js_value_type(value) != fiber::script::JsNodeType::String) {
        return 0;
    }
    if (fiber::script::js_value_is_borrowed_string(value)) {
        return fiber::script::js_value_native_string(value).len;
    }
    auto *str = fiber::script::js_value_heap_ptr<const fiber::script::GcString>(value);
    return str ? str->len : 0;
}

std::size_t primitive_string_code_unit_upper_bound(const fiber::script::JsValue &value) {
    switch (fiber::script::js_value_type(value)) {
        case fiber::script::JsNodeType::String:
            return string_code_unit_upper_bound(value);
        case fiber::script::JsNodeType::Undefined:
            return 9;
        case fiber::script::JsNodeType::Null:
            return 4;
        case fiber::script::JsNodeType::Boolean:
            return fiber::script::js_value_bool(value) ? 4 : 5;
        case fiber::script::JsNodeType::Integer: {
            char buffer[64];
            auto converted = std::to_chars(buffer, buffer + sizeof(buffer), fiber::script::js_value_int64(value));
            return converted.ec == std::errc{} ? static_cast<std::size_t>(converted.ptr - buffer) : 0;
        }
        case fiber::script::JsNodeType::Float: {
            double number = fiber::script::js_value_double(value);
            if (std::isnan(number)) {
                return 3;
            }
            if (std::isinf(number)) {
                return number < 0 ? 9 : 8;
            }
            char buffer[64];
            auto converted = std::to_chars(buffer, buffer + sizeof(buffer), number);
            return converted.ec == std::errc{} ? static_cast<std::size_t>(converted.ptr - buffer) : 0;
        }
        case fiber::script::JsNodeType::Array:
        case fiber::script::JsNodeType::Object:
        case fiber::script::JsNodeType::Interator:
        case fiber::script::JsNodeType::Exception:
        case fiber::script::JsNodeType::Binary:
            return 0;
    }
    return 0;
}

std::size_t estimate_plus_alloc_bytes(const fiber::script::JsValue &lhs, const fiber::script::JsValue &rhs) {
    if (!is_string_like(lhs) && !is_string_like(rhs)) {
        return 0;
    }
    std::size_t total_units = primitive_string_code_unit_upper_bound(lhs) + primitive_string_code_unit_upper_bound(rhs);
    bool all_byte = fiber::script::js_value_type(lhs) == fiber::script::JsNodeType::String &&
                    fiber::script::js_value_type(rhs) == fiber::script::JsNodeType::String &&
                    !fiber::script::js_value_is_borrowed_string(lhs) &&
                    !fiber::script::js_value_is_borrowed_string(rhs);
    if (all_byte) {
        auto *lhs_str = fiber::script::js_value_heap_ptr<const fiber::script::GcString>(lhs);
        auto *rhs_str = fiber::script::js_value_heap_ptr<const fiber::script::GcString>(rhs);
        all_byte = lhs_str && rhs_str && lhs_str->encoding == fiber::script::GcStringEncoding::Byte &&
                   rhs_str->encoding == fiber::script::GcStringEncoding::Byte;
    }
    return all_byte ? fiber::script::gc_estimate_string_bytes(total_units, fiber::script::GcStringEncoding::Byte)
                    : fiber::script::gc_estimate_string_bytes(total_units, fiber::script::GcStringEncoding::Utf16);
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
    fiber::json::Utf8ScanResult scan = {};
};

void set_ascii_string_source(StringSource &out, const char *data, std::size_t len) noexcept {
    out.kind = StringKind::NativeUtf8;
    out.utf8 = data;
    out.len = len;
    out.scan.all_byte = true;
    out.scan.utf16_len = len;
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
        if (str->encoding == fiber::script::GcStringEncoding::Byte) {
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
    fiber::script::NativeStr native = fiber::script::js_value_native_string(value);
    out.kind = StringKind::NativeUtf8;
    out.utf8 = native.data;
    out.len = native.len;
    if (!fiber::json::utf8_scan(out.utf8, out.len, out.scan)) {
        return false;
    }
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
        fiber::script::JsValue out = fiber::script::JsValue::make_string(heap, "", 0);
        if (fiber::script::js_value_type(out) != fiber::script::JsNodeType::String ||
            fiber::script::js_value_is_borrowed_string(out)) {
            return set_abort(result, ScriptAbortReason::OutOfMemory);
        }
        return set_value(result, out);
    }

    if (all_byte) {
        fiber::script::GcString *result_str = fiber::script::gc_new_string_bytes_uninit(&heap, total_len);
        if (!result_str) {
            return set_abort(result, ScriptAbortReason::OutOfMemory);
        }
        std::uint8_t *dst = result_str->data8;
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
                    if (!fiber::json::utf8_write_bytes(part.utf8, part.len, dst + offset, part.scan.utf16_len)) {
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
            return set_exception(result, fiber::script::ExceptionKind::TypeError);
        }
        return set_value(result, fiber::script::js_make_heap_ref(&result_str->hdr, fiber::script::JsHeapKind::String));
    }

    fiber::script::GcString *result_str = fiber::script::gc_new_string_utf16_uninit(&heap, total_len);
    if (!result_str) {
        return set_abort(result, ScriptAbortReason::OutOfMemory);
    }
    char16_t *dst = result_str->data16;
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
                if (!fiber::json::utf8_write_utf16(part.utf8, part.len, dst + offset, part.scan.utf16_len)) {
                    return false;
                }
                offset += part.scan.utf16_len;
                return true;
        }
        return false;
    };
    if (!append_part(lhs) || !append_part(rhs)) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    return set_value(result, fiber::script::js_make_heap_ref(&result_str->hdr, fiber::script::JsHeapKind::String));
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
    CallResult status = CallResult::Success;
    runtime.run_with_gc_retry(estimate_plus_alloc_bytes(*a, *b), [&]() {
        status = plus_impl(runtime.heap(), *a, *b, result);
        return status != CallResult::Abort;
    });
    return status;
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
