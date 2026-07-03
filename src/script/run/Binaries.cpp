#include "Binaries.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "../../common/json/Utf.h"
#include "../Runtime.h"
#include "Compares.h"

namespace fiber::script::run {

namespace {

ScriptStatus store_value(ValueHandle out, const fiber::json::JsValue &value) noexcept {
    if (!out) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
    }
    *out = value;
    return ScriptStatus::success();
}

bool is_string_like(const fiber::json::JsValue &value) noexcept {
    return fiber::json::js_value_type(value) == fiber::json::JsNodeType::String;
}

bool is_numeric_like(fiber::json::JsNodeType type) noexcept {
    return type == fiber::json::JsNodeType::Integer || type == fiber::json::JsNodeType::Float ||
           type == fiber::json::JsNodeType::Boolean || type == fiber::json::JsNodeType::Null;
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

bool to_int64(const fiber::json::JsValue &value, std::int64_t &out) noexcept {
    switch (fiber::json::js_value_type(value)) {
        case fiber::json::JsNodeType::Integer:
            out = fiber::json::js_value_int64(value);
            return true;
        case fiber::json::JsNodeType::Float:
            out = static_cast<std::int64_t>(fiber::json::js_value_double(value));
            return true;
        case fiber::json::JsNodeType::Boolean:
            out = fiber::json::js_value_bool(value) ? 1 : 0;
            return true;
        case fiber::json::JsNodeType::Null:
            out = 0;
            return true;
        default:
            return false;
    }
}

const fiber::json::GcString *as_heap_string(const fiber::json::JsValue &value) noexcept {
    return fiber::json::js_value_type(value) == fiber::json::JsNodeType::String
                   ? fiber::json::js_value_heap_ptr<const fiber::json::GcString>(value)
                   : nullptr;
}

std::size_t string_code_unit_upper_bound(const fiber::json::JsValue &value) noexcept {
    if (fiber::json::js_value_type(value) != fiber::json::JsNodeType::String) {
        return 0;
    }
    if (fiber::json::js_value_is_borrowed_string(value)) {
        return fiber::json::js_value_native_string(value).len;
    }
    auto *str = fiber::json::js_value_heap_ptr<const fiber::json::GcString>(value);
    return str ? str->len : 0;
}

std::size_t primitive_string_code_unit_upper_bound(const fiber::json::JsValue &value) {
    switch (fiber::json::js_value_type(value)) {
        case fiber::json::JsNodeType::String:
            return string_code_unit_upper_bound(value);
        case fiber::json::JsNodeType::Undefined:
            return 9;
        case fiber::json::JsNodeType::Null:
            return 4;
        case fiber::json::JsNodeType::Boolean:
            return fiber::json::js_value_bool(value) ? 4 : 5;
        case fiber::json::JsNodeType::Integer: {
            char buffer[64];
            auto converted = std::to_chars(buffer, buffer + sizeof(buffer), fiber::json::js_value_int64(value));
            return converted.ec == std::errc{} ? static_cast<std::size_t>(converted.ptr - buffer) : 0;
        }
        case fiber::json::JsNodeType::Float: {
            double number = fiber::json::js_value_double(value);
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
        case fiber::json::JsNodeType::Array:
        case fiber::json::JsNodeType::Object:
        case fiber::json::JsNodeType::Interator:
        case fiber::json::JsNodeType::Exception:
        case fiber::json::JsNodeType::Binary:
            return 0;
    }
    return 0;
}

std::size_t estimate_plus_alloc_bytes(const fiber::json::JsValue &lhs, const fiber::json::JsValue &rhs) {
    if (!is_string_like(lhs) && !is_string_like(rhs)) {
        return 0;
    }
    std::size_t total_units = primitive_string_code_unit_upper_bound(lhs) + primitive_string_code_unit_upper_bound(rhs);
    bool all_byte = fiber::json::js_value_type(lhs) == fiber::json::JsNodeType::String &&
                    fiber::json::js_value_type(rhs) == fiber::json::JsNodeType::String &&
                    !fiber::json::js_value_is_borrowed_string(lhs) && !fiber::json::js_value_is_borrowed_string(rhs);
    if (all_byte) {
        auto *lhs_str = fiber::json::js_value_heap_ptr<const fiber::json::GcString>(lhs);
        auto *rhs_str = fiber::json::js_value_heap_ptr<const fiber::json::GcString>(rhs);
        all_byte = lhs_str && rhs_str && lhs_str->encoding == fiber::json::GcStringEncoding::Byte &&
                   rhs_str->encoding == fiber::json::GcStringEncoding::Byte;
    }
    return all_byte ? fiber::json::gc_estimate_string_bytes(total_units, fiber::json::GcStringEncoding::Byte)
                    : fiber::json::gc_estimate_string_bytes(total_units, fiber::json::GcStringEncoding::Utf16);
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

bool build_string_source(const fiber::json::JsValue &value, StringSource &out, ScriptAbortReason &error) {
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
            out.len = str->len;
        } else {
            out.kind = StringKind::HeapUtf16;
            out.u16 = str->data16;
            out.len = str->len;
        }
        return true;
    }
    fiber::json::NativeStr native = fiber::json::js_value_native_string(value);
    out.kind = StringKind::NativeUtf8;
    out.utf8 = native.data;
    out.len = native.len;
    if (!fiber::json::utf8_scan(out.utf8, out.len, out.scan)) {
        error = ScriptAbortReason::InvalidArgument;
        return false;
    }
    return true;
}

bool primitive_to_string_source(const fiber::json::JsValue &value, StringSource &out, char *buffer,
                                std::size_t buffer_len, ScriptAbortReason &error) {
    if (fiber::json::js_value_type(value) == fiber::json::JsNodeType::String) {
        return build_string_source(value, out, error);
    }
    switch (fiber::json::js_value_type(value)) {
        case fiber::json::JsNodeType::Undefined:
            set_ascii_string_source(out, "undefined", 9);
            return true;
        case fiber::json::JsNodeType::Null:
            set_ascii_string_source(out, "null", 4);
            return true;
        case fiber::json::JsNodeType::Boolean:
            if (fiber::json::js_value_bool(value)) {
                set_ascii_string_source(out, "true", 4);
            } else {
                set_ascii_string_source(out, "false", 5);
            }
            return true;
        case fiber::json::JsNodeType::Integer: {
            auto converted = std::to_chars(buffer, buffer + buffer_len, fiber::json::js_value_int64(value));
            if (converted.ec != std::errc{}) {
                error = ScriptAbortReason::TypeError;
                return false;
            }
            set_ascii_string_source(out, buffer, static_cast<std::size_t>(converted.ptr - buffer));
            return true;
        }
        case fiber::json::JsNodeType::Float: {
            double number = fiber::json::js_value_double(value);
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
                error = ScriptAbortReason::TypeError;
                return false;
            }
            set_ascii_string_source(out, buffer, static_cast<std::size_t>(converted.ptr - buffer));
            return true;
        }
        case fiber::json::JsNodeType::String:
            return build_string_source(value, out, error);
        case fiber::json::JsNodeType::Array:
        case fiber::json::JsNodeType::Object:
        case fiber::json::JsNodeType::Interator:
        case fiber::json::JsNodeType::Exception:
        case fiber::json::JsNodeType::Binary:
            error = ScriptAbortReason::TypeError;
            return false;
    }
    error = ScriptAbortReason::TypeError;
    return false;
}

ScriptStatus concat_strings(fiber::json::GcHeap &heap, const StringSource &lhs, const StringSource &rhs,
                            fiber::json::JsValue &out) {
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
        out = fiber::json::JsValue::make_string(heap, "", 0);
        if (fiber::json::js_value_type(out) != fiber::json::JsNodeType::String ||
            fiber::json::js_value_is_borrowed_string(out)) {
            return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
        }
        return ScriptStatus::success();
    }

    if (all_byte) {
        fiber::json::GcString *result = fiber::json::gc_new_string_bytes_uninit(&heap, total_len);
        if (!result) {
            return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
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
            return ScriptStatus::abort(ScriptAbortReason::InvalidArgument);
        }
        out = fiber::json::js_make_heap_ref(&result->hdr, fiber::json::JsHeapKind::String);
        return ScriptStatus::success();
    }

    fiber::json::GcString *result = fiber::json::gc_new_string_utf16_uninit(&heap, total_len);
    if (!result) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
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
                if (!fiber::json::utf8_write_utf16(part.utf8, part.len, dst + offset, part.scan.utf16_len)) {
                    return false;
                }
                offset += part.scan.utf16_len;
                return true;
        }
        return false;
    };
    if (!append_part(lhs) || !append_part(rhs)) {
        return ScriptStatus::abort(ScriptAbortReason::InvalidArgument);
    }
    out = fiber::json::js_make_heap_ref(&result->hdr, fiber::json::JsHeapKind::String);
    return ScriptStatus::success();
}

ScriptStatus add_numeric(const fiber::json::JsValue &lhs, const fiber::json::JsValue &rhs,
                         fiber::json::JsValue &out) noexcept {
    if (!is_numeric_like(fiber::json::js_value_type(lhs)) || !is_numeric_like(fiber::json::js_value_type(rhs))) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    if (fiber::json::js_value_type(lhs) == fiber::json::JsNodeType::Float ||
        fiber::json::js_value_type(rhs) == fiber::json::JsNodeType::Float) {
        double a = 0.0;
        double b = 0.0;
        if (!to_number(lhs, a) || !to_number(rhs, b)) {
            return ScriptStatus::abort(ScriptAbortReason::TypeError);
        }
        out = fiber::json::JsValue::make_float(a + b);
        return ScriptStatus::success();
    }
    std::int64_t a = 0;
    std::int64_t b = 0;
    if (!to_int64(lhs, a) || !to_int64(rhs, b)) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    std::int64_t result = 0;
    if (!__builtin_add_overflow(a, b, &result)) {
        out = fiber::json::JsValue::make_integer(result);
        return ScriptStatus::success();
    }
    out = fiber::json::JsValue::make_float(static_cast<double>(a) + static_cast<double>(b));
    return ScriptStatus::success();
}

ScriptStatus sub_numeric(const fiber::json::JsValue &lhs, const fiber::json::JsValue &rhs,
                         fiber::json::JsValue &out) noexcept {
    if (!is_numeric_like(fiber::json::js_value_type(lhs)) || !is_numeric_like(fiber::json::js_value_type(rhs))) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    if (fiber::json::js_value_type(lhs) == fiber::json::JsNodeType::Float ||
        fiber::json::js_value_type(rhs) == fiber::json::JsNodeType::Float) {
        double a = 0.0;
        double b = 0.0;
        if (!to_number(lhs, a) || !to_number(rhs, b)) {
            return ScriptStatus::abort(ScriptAbortReason::TypeError);
        }
        out = fiber::json::JsValue::make_float(a - b);
        return ScriptStatus::success();
    }
    std::int64_t a = 0;
    std::int64_t b = 0;
    if (!to_int64(lhs, a) || !to_int64(rhs, b)) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    std::int64_t result = 0;
    if (!__builtin_sub_overflow(a, b, &result)) {
        out = fiber::json::JsValue::make_integer(result);
        return ScriptStatus::success();
    }
    out = fiber::json::JsValue::make_float(static_cast<double>(a) - static_cast<double>(b));
    return ScriptStatus::success();
}

ScriptStatus mul_numeric(const fiber::json::JsValue &lhs, const fiber::json::JsValue &rhs,
                         fiber::json::JsValue &out) noexcept {
    if (!is_numeric_like(fiber::json::js_value_type(lhs)) || !is_numeric_like(fiber::json::js_value_type(rhs))) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    if (fiber::json::js_value_type(lhs) == fiber::json::JsNodeType::Float ||
        fiber::json::js_value_type(rhs) == fiber::json::JsNodeType::Float) {
        double a = 0.0;
        double b = 0.0;
        if (!to_number(lhs, a) || !to_number(rhs, b)) {
            return ScriptStatus::abort(ScriptAbortReason::TypeError);
        }
        out = fiber::json::JsValue::make_float(a * b);
        return ScriptStatus::success();
    }
    std::int64_t a = 0;
    std::int64_t b = 0;
    if (!to_int64(lhs, a) || !to_int64(rhs, b)) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    std::int64_t result = 0;
    if (!__builtin_mul_overflow(a, b, &result)) {
        out = fiber::json::JsValue::make_integer(result);
        return ScriptStatus::success();
    }
    out = fiber::json::JsValue::make_float(static_cast<double>(a) * static_cast<double>(b));
    return ScriptStatus::success();
}

ScriptStatus plus_impl(fiber::json::GcHeap &heap, const fiber::json::JsValue &lhs, const fiber::json::JsValue &rhs,
                       fiber::json::JsValue &out) {
    if (is_string_like(lhs) || is_string_like(rhs)) {
        StringSource lhs_src;
        StringSource rhs_src;
        char lhs_buf[64];
        char rhs_buf[64];
        ScriptAbortReason error = ScriptAbortReason::None;
        if (!primitive_to_string_source(lhs, lhs_src, lhs_buf, sizeof(lhs_buf), error)) {
            return ScriptStatus::abort(error);
        }
        if (!primitive_to_string_source(rhs, rhs_src, rhs_buf, sizeof(rhs_buf), error)) {
            return ScriptStatus::abort(error);
        }
        return concat_strings(heap, lhs_src, rhs_src, out);
    }
    return add_numeric(lhs, rhs, out);
}

} // namespace

ScriptStatus Binaries::plus(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    if (!out) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
    }
    fiber::json::JsValue result = fiber::json::JsValue::make_undefined();
    ScriptStatus status = ScriptStatus::success();
    runtime.run_with_gc_retry(estimate_plus_alloc_bytes(*a, *b), [&]() {
        status = plus_impl(runtime.heap(), *a, *b, result);
        return !status.is_abort() || status.abort().reason != ScriptAbortReason::OutOfMemory;
    });
    if (!status) {
        return status;
    }
    *out = result;
    return ScriptStatus::success();
}

ScriptStatus Binaries::minus(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    (void) runtime;
    fiber::json::JsValue result = fiber::json::JsValue::make_undefined();
    ScriptStatus status = sub_numeric(*a, *b, result);
    if (!status) {
        return status;
    }
    return store_value(out, result);
}

ScriptStatus Binaries::multiply(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a,
                                ConstValueHandle b) noexcept {
    (void) runtime;
    fiber::json::JsValue result = fiber::json::JsValue::make_undefined();
    ScriptStatus status = mul_numeric(*a, *b, result);
    if (!status) {
        return status;
    }
    return store_value(out, result);
}

ScriptStatus Binaries::divide(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a,
                              ConstValueHandle b) noexcept {
    (void) runtime;
    if (!is_numeric_like(fiber::json::js_value_type(*a)) || !is_numeric_like(fiber::json::js_value_type(*b))) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    double lhs = 0.0;
    double rhs = 0.0;
    if (!to_number(*a, lhs) || !to_number(*b, rhs)) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    if (rhs == 0.0) {
        return ScriptStatus::abort(ScriptAbortReason::DivisionByZero);
    }
    return store_value(out, fiber::json::JsValue::make_float(lhs / rhs));
}

ScriptStatus Binaries::modulo(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a,
                              ConstValueHandle b) noexcept {
    (void) runtime;
    if (!is_numeric_like(fiber::json::js_value_type(*a)) || !is_numeric_like(fiber::json::js_value_type(*b))) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    if (fiber::json::js_value_type(*a) == fiber::json::JsNodeType::Float ||
        fiber::json::js_value_type(*b) == fiber::json::JsNodeType::Float) {
        double lhs = 0.0;
        double rhs = 0.0;
        if (!to_number(*a, lhs) || !to_number(*b, rhs)) {
            return ScriptStatus::abort(ScriptAbortReason::TypeError);
        }
        if (rhs == 0.0) {
            return ScriptStatus::abort(ScriptAbortReason::DivisionByZero);
        }
        return store_value(out, fiber::json::JsValue::make_float(std::fmod(lhs, rhs)));
    }
    std::int64_t lhs = 0;
    std::int64_t rhs = 0;
    if (!to_int64(*a, lhs) || !to_int64(*b, rhs)) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    if (rhs == 0) {
        return ScriptStatus::abort(ScriptAbortReason::DivisionByZero);
    }
    return store_value(out, fiber::json::JsValue::make_integer(lhs % rhs));
}

ScriptStatus Binaries::matches(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a,
                               ConstValueHandle b) noexcept {
    (void) runtime;
    return store_value(out, fiber::json::JsValue::make_boolean(Compares::matches(a, b)));
}

ScriptStatus Binaries::lt(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    (void) runtime;
    return store_value(out, fiber::json::JsValue::make_boolean(Compares::lt(a, b)));
}

ScriptStatus Binaries::lte(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    (void) runtime;
    return store_value(out, fiber::json::JsValue::make_boolean(Compares::lte(a, b)));
}

ScriptStatus Binaries::gt(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    (void) runtime;
    return store_value(out, fiber::json::JsValue::make_boolean(Compares::gt(a, b)));
}

ScriptStatus Binaries::gte(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    (void) runtime;
    return store_value(out, fiber::json::JsValue::make_boolean(Compares::gte(a, b)));
}

ScriptStatus Binaries::eq(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    (void) runtime;
    return store_value(out, fiber::json::JsValue::make_boolean(Compares::eq(a, b)));
}

ScriptStatus Binaries::seq(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    (void) runtime;
    return store_value(out, fiber::json::JsValue::make_boolean(Compares::seq(a, b)));
}

ScriptStatus Binaries::ne(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    (void) runtime;
    return store_value(out, fiber::json::JsValue::make_boolean(Compares::ne(a, b)));
}

ScriptStatus Binaries::sne(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    (void) runtime;
    return store_value(out, fiber::json::JsValue::make_boolean(Compares::sne(a, b)));
}

ScriptStatus Binaries::in(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    (void) runtime;
    return store_value(out, fiber::json::JsValue::make_boolean(Compares::in(a, b)));
}

} // namespace fiber::script::run
