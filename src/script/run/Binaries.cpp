#include "Binaries.h"

#include <charconv>
#include <cmath>
#include <string>
#include <string_view>

#include "../../common/json/JsValueOps.h"
#include "../Runtime.h"
#include "Compares.h"

namespace fiber::script::run {

namespace {

ScriptAbortReason map_error(fiber::json::JsOpError error) {
    switch (error) {
        case fiber::json::JsOpError::TypeError:
            return ScriptAbortReason::TypeError;
        case fiber::json::JsOpError::DivisionByZero:
            return ScriptAbortReason::DivisionByZero;
        case fiber::json::JsOpError::HeapRequired:
            return ScriptAbortReason::InvalidState;
        case fiber::json::JsOpError::OutOfMemory:
            return ScriptAbortReason::OutOfMemory;
        case fiber::json::JsOpError::InvalidUtf8:
            return ScriptAbortReason::InvalidArgument;
        case fiber::json::JsOpError::None:
            return ScriptAbortReason::Internal;
    }
    return ScriptAbortReason::Internal;
}

ScriptStatus from_js_result(ValueHandle out, const fiber::json::JsOpResult &result) noexcept {
    if (!out) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
    }
    if (result.error == fiber::json::JsOpError::None) {
        *out = result.value;
        return ScriptStatus::success();
    }
    return ScriptStatus::abort(map_error(result.error));
}

bool value_to_string(const fiber::json::JsValue &value, std::string &out) {
    if (fiber::json::js_value_type(value) != fiber::json::JsNodeType::String) {
        return false;
    }
    if (fiber::json::js_value_is_borrowed_string(value)) {
        fiber::json::NativeStr native = fiber::json::js_value_native_string(value);
        out.assign(native.data, native.len);
        return true;
    }
    auto *str = fiber::json::js_value_heap_ptr<const fiber::json::GcString>(value);
    return fiber::json::gc_string_to_utf8(str, out);
}

ScriptStatus make_bool(ValueHandle out, bool value) noexcept {
    if (!out) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
    }
    *out = fiber::json::JsValue::make_boolean(value);
    return ScriptStatus::success();
}

bool is_string_like(const fiber::json::JsValue &value) {
    return fiber::json::js_value_type(value) == fiber::json::JsNodeType::String;
}

std::size_t string_code_unit_upper_bound(const fiber::json::JsValue &value) {
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

} // namespace

ScriptStatus Binaries::plus(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    fiber::json::JsOpResult result;
    runtime.run_with_gc_retry(estimate_plus_alloc_bytes(*a, *b), [&]() {
        result = fiber::json::js_binary_op(fiber::json::JsBinaryOp::Add, *a, *b, &runtime.heap());
        return result.error != fiber::json::JsOpError::OutOfMemory;
    });
    return from_js_result(out, result);
}

ScriptStatus Binaries::minus(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Sub, *a, *b, &runtime.heap()));
}

ScriptStatus Binaries::multiply(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a,
                                ConstValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Mul, *a, *b, &runtime.heap()));
}

ScriptStatus Binaries::divide(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a,
                              ConstValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Div, *a, *b, &runtime.heap()));
}

ScriptStatus Binaries::modulo(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a,
                              ConstValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Mod, *a, *b, &runtime.heap()));
}

ScriptStatus Binaries::matches(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a,
                               ConstValueHandle b) noexcept {
    (void) a;
    (void) b;
    (void) runtime;
    return make_bool(out, false);
}

ScriptStatus Binaries::lt(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Lt, *a, *b, &runtime.heap()));
}

ScriptStatus Binaries::lte(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Le, *a, *b, &runtime.heap()));
}

ScriptStatus Binaries::gt(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Gt, *a, *b, &runtime.heap()));
}

ScriptStatus Binaries::gte(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Ge, *a, *b, &runtime.heap()));
}

ScriptStatus Binaries::eq(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Eq, *a, *b, &runtime.heap()));
}

ScriptStatus Binaries::seq(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::StrictEq, *a, *b, &runtime.heap()));
}

ScriptStatus Binaries::ne(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Ne, *a, *b, &runtime.heap()));
}

ScriptStatus Binaries::sne(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::StrictNe, *a, *b, &runtime.heap()));
}

ScriptStatus Binaries::in(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept {
    (void) runtime;
    return Compares::in(out, a, b);
}

} // namespace fiber::script::run
