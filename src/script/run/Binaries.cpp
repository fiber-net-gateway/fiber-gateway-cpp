#include "Binaries.h"

#include <string>
#include <string_view>

#include "../../common/json/JsValueOps.h"
#include "../Runtime.h"
#include "Compares.h"

namespace fiber::script::run {

namespace {

VmError map_error(fiber::json::JsOpError error, std::string_view op) {
    VmError vm_error;
    vm_error.status = 500;
    switch (error) {
        case fiber::json::JsOpError::TypeError:
            vm_error.name = "EXEC_TYPE_ERROR";
            vm_error.message = "type error in operator ";
            break;
        case fiber::json::JsOpError::DivisionByZero:
            vm_error.name = "EXEC_DIVISION_BY_ZERO";
            vm_error.message = "division by zero in operator ";
            break;
        case fiber::json::JsOpError::HeapRequired:
            vm_error.name = "EXEC_HEAP_REQUIRED";
            vm_error.message = "heap required in operator ";
            break;
        case fiber::json::JsOpError::OutOfMemory:
            vm_error.name = "EXEC_OUT_OF_MEMORY";
            vm_error.message = "out of memory in operator ";
            break;
        case fiber::json::JsOpError::InvalidUtf8:
            vm_error.name = "EXEC_INVALID_UTF8";
            vm_error.message = "invalid utf-8 in operator ";
            break;
        case fiber::json::JsOpError::None:
            vm_error.name = "EXEC_ERROR";
            vm_error.message = "unknown error in operator ";
            break;
    }
    vm_error.message += op;
    return vm_error;
}

VmResult from_js_result(const fiber::json::JsOpResult &result, std::string_view op) {
    if (result.error == fiber::json::JsOpError::None) {
        return result.value;
    }
    return std::unexpected(map_error(result.error, op));
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

VmResult make_bool(bool value) {
    return fiber::json::JsValue::make_boolean(value);
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

std::size_t estimate_plus_alloc_bytes(const fiber::json::JsValue &lhs, const fiber::json::JsValue &rhs) {
    if (!is_string_like(lhs) && !is_string_like(rhs)) {
        return 0;
    }
    std::size_t total_units = string_code_unit_upper_bound(lhs) + string_code_unit_upper_bound(rhs);
    bool all_byte = fiber::json::js_value_type(lhs) == fiber::json::JsNodeType::String &&
                    fiber::json::js_value_type(rhs) == fiber::json::JsNodeType::String &&
                    !fiber::json::js_value_is_borrowed_string(lhs) &&
                    !fiber::json::js_value_is_borrowed_string(rhs);
    if (all_byte) {
        auto *lhs_str = fiber::json::js_value_heap_ptr<const fiber::json::GcString>(lhs);
        auto *rhs_str = fiber::json::js_value_heap_ptr<const fiber::json::GcString>(rhs);
        all_byte = lhs_str && rhs_str &&
                   lhs_str->encoding == fiber::json::GcStringEncoding::Byte &&
                   rhs_str->encoding == fiber::json::GcStringEncoding::Byte;
    }
    return all_byte ? fiber::json::gc_estimate_string_bytes(total_units, fiber::json::GcStringEncoding::Byte)
                    : fiber::json::gc_estimate_string_bytes(total_units, fiber::json::GcStringEncoding::Utf16);
}

} // namespace

VmResult Binaries::plus(const fiber::json::JsValue &a,
                        const fiber::json::JsValue &b,
                        ScriptRuntime &runtime) {
    fiber::json::JsOpResult result;
    runtime.run_with_gc_retry(estimate_plus_alloc_bytes(a, b), [&]() {
        result = fiber::json::js_binary_op(fiber::json::JsBinaryOp::Add, a, b, &runtime.heap());
        return result.error != fiber::json::JsOpError::OutOfMemory;
    });
    return from_js_result(result, "+");
}

VmResult Binaries::minus(const fiber::json::JsValue &a,
                         const fiber::json::JsValue &b,
                         ScriptRuntime &runtime) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Sub, a, b, &runtime.heap()), "-");
}

VmResult Binaries::multiply(const fiber::json::JsValue &a,
                            const fiber::json::JsValue &b,
                            ScriptRuntime &runtime) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Mul, a, b, &runtime.heap()), "*");
}

VmResult Binaries::divide(const fiber::json::JsValue &a,
                          const fiber::json::JsValue &b,
                          ScriptRuntime &runtime) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Div, a, b, &runtime.heap()), "/");
}

VmResult Binaries::modulo(const fiber::json::JsValue &a,
                          const fiber::json::JsValue &b,
                          ScriptRuntime &runtime) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Mod, a, b, &runtime.heap()), "%");
}

VmResult Binaries::matches(const fiber::json::JsValue &a,
                           const fiber::json::JsValue &b,
                           ScriptRuntime &runtime) {
    (void)a;
    (void)b;
    (void)runtime;
    return make_bool(false);
}

VmResult Binaries::lt(const fiber::json::JsValue &a,
                      const fiber::json::JsValue &b,
                      ScriptRuntime &runtime) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Lt, a, b, &runtime.heap()), "<");
}

VmResult Binaries::lte(const fiber::json::JsValue &a,
                       const fiber::json::JsValue &b,
                       ScriptRuntime &runtime) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Le, a, b, &runtime.heap()), "<=");
}

VmResult Binaries::gt(const fiber::json::JsValue &a,
                      const fiber::json::JsValue &b,
                      ScriptRuntime &runtime) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Gt, a, b, &runtime.heap()), ">");
}

VmResult Binaries::gte(const fiber::json::JsValue &a,
                       const fiber::json::JsValue &b,
                       ScriptRuntime &runtime) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Ge, a, b, &runtime.heap()), ">=");
}

VmResult Binaries::eq(const fiber::json::JsValue &a,
                      const fiber::json::JsValue &b,
                      ScriptRuntime &runtime) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Eq, a, b, &runtime.heap()), "==");
}

VmResult Binaries::seq(const fiber::json::JsValue &a,
                       const fiber::json::JsValue &b,
                       ScriptRuntime &runtime) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::StrictEq, a, b, &runtime.heap()), "===");
}

VmResult Binaries::ne(const fiber::json::JsValue &a,
                      const fiber::json::JsValue &b,
                      ScriptRuntime &runtime) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Ne, a, b, &runtime.heap()), "!=");
}

VmResult Binaries::sne(const fiber::json::JsValue &a,
                       const fiber::json::JsValue &b,
                       ScriptRuntime &runtime) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::StrictNe, a, b, &runtime.heap()), "!==");
}

VmResult Binaries::in(const fiber::json::JsValue &a,
                      const fiber::json::JsValue &b,
                      ScriptRuntime &runtime) {
    return Compares::in(a, b);
}

} // namespace fiber::script::run
