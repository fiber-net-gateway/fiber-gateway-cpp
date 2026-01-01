#include "Binaries.h"

#include <string>
#include <string_view>

#include "../../common/json/JsValueOps.h"
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
    if (value.type_ == fiber::json::JsNodeType::NativeString) {
        out.assign(value.ns.data, value.ns.len);
        return true;
    }
    if (value.type_ == fiber::json::JsNodeType::HeapString) {
        auto *str = reinterpret_cast<const fiber::json::GcString *>(value.gc);
        return fiber::json::gc_string_to_utf8(str, out);
    }
    return false;
}

VmResult make_bool(bool value) {
    return fiber::json::JsValue::make_boolean(value);
}

} // namespace

VmResult Binaries::plus(const fiber::json::JsValue &a,
                        const fiber::json::JsValue &b,
                        fiber::json::GcHeap *heap) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Add, a, b, heap), "+");
}

VmResult Binaries::minus(const fiber::json::JsValue &a,
                         const fiber::json::JsValue &b,
                         fiber::json::GcHeap *heap) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Sub, a, b, heap), "-");
}

VmResult Binaries::multiply(const fiber::json::JsValue &a,
                            const fiber::json::JsValue &b,
                            fiber::json::GcHeap *heap) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Mul, a, b, heap), "*");
}

VmResult Binaries::divide(const fiber::json::JsValue &a,
                          const fiber::json::JsValue &b,
                          fiber::json::GcHeap *heap) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Div, a, b, heap), "/");
}

VmResult Binaries::modulo(const fiber::json::JsValue &a,
                          const fiber::json::JsValue &b,
                          fiber::json::GcHeap *heap) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Mod, a, b, heap), "%");
}

VmResult Binaries::matches(const fiber::json::JsValue &a,
                           const fiber::json::JsValue &b,
                           fiber::json::GcHeap *heap) {
    (void)a;
    (void)b;
    (void)heap;
    return make_bool(false);
}

VmResult Binaries::lt(const fiber::json::JsValue &a,
                      const fiber::json::JsValue &b,
                      fiber::json::GcHeap *heap) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Lt, a, b, heap), "<");
}

VmResult Binaries::lte(const fiber::json::JsValue &a,
                       const fiber::json::JsValue &b,
                       fiber::json::GcHeap *heap) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Le, a, b, heap), "<=");
}

VmResult Binaries::gt(const fiber::json::JsValue &a,
                      const fiber::json::JsValue &b,
                      fiber::json::GcHeap *heap) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Gt, a, b, heap), ">");
}

VmResult Binaries::gte(const fiber::json::JsValue &a,
                       const fiber::json::JsValue &b,
                       fiber::json::GcHeap *heap) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Ge, a, b, heap), ">=");
}

VmResult Binaries::eq(const fiber::json::JsValue &a,
                      const fiber::json::JsValue &b,
                      fiber::json::GcHeap *heap) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Eq, a, b, heap), "==");
}

VmResult Binaries::seq(const fiber::json::JsValue &a,
                       const fiber::json::JsValue &b,
                       fiber::json::GcHeap *heap) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::StrictEq, a, b, heap), "===");
}

VmResult Binaries::ne(const fiber::json::JsValue &a,
                      const fiber::json::JsValue &b,
                      fiber::json::GcHeap *heap) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Ne, a, b, heap), "!=");
}

VmResult Binaries::sne(const fiber::json::JsValue &a,
                       const fiber::json::JsValue &b,
                       fiber::json::GcHeap *heap) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::StrictNe, a, b, heap), "!==");
}

VmResult Binaries::in(const fiber::json::JsValue &a,
                      const fiber::json::JsValue &b,
                      fiber::json::GcHeap *heap) {
    (void)heap;
    return Compares::in(a, b);
}

} // namespace fiber::script::run
