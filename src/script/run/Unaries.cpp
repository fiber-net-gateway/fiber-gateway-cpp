#include "Unaries.h"

#include "../../common/json/JsValueOps.h"
#include "../Runtime.h"

#include <cstring>
#include <string_view>

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

VmResult make_typeof_value(const char *text) {
    fiber::json::JsValue value = fiber::json::JsValue::make_native_string(const_cast<char *>(text), std::strlen(text));
    return value;
}

} // namespace

VmResult Unaries::neg(const fiber::json::JsValue &value) {
    return from_js_result(fiber::json::js_unary_op(fiber::json::JsUnaryOp::LogicalNot, value), "!");
}

VmResult Unaries::plus(const fiber::json::JsValue &value) {
    return from_js_result(fiber::json::js_unary_op(fiber::json::JsUnaryOp::Plus, value), "+");
}

VmResult Unaries::minus(const fiber::json::JsValue &value) {
    return from_js_result(fiber::json::js_unary_op(fiber::json::JsUnaryOp::Negate, value), "-");
}

VmResult Unaries::typeof_op(const fiber::json::JsValue &value, ScriptRuntime &runtime) {
    (void)runtime;
    switch (value.type_) {
        case fiber::json::JsNodeType::Undefined:
            return make_typeof_value("undefined");
        case fiber::json::JsNodeType::Null:
            return make_typeof_value("null");
        case fiber::json::JsNodeType::Boolean:
            return make_typeof_value("boolean");
        case fiber::json::JsNodeType::Integer:
        case fiber::json::JsNodeType::Float:
            return make_typeof_value("number");
        case fiber::json::JsNodeType::HeapString:
        case fiber::json::JsNodeType::NativeString:
            return make_typeof_value("string");
        case fiber::json::JsNodeType::Array:
            return make_typeof_value("array");
        case fiber::json::JsNodeType::Object:
            return make_typeof_value("object");
        case fiber::json::JsNodeType::Interator:
            return make_typeof_value("iterator");
        case fiber::json::JsNodeType::Exception:
            return make_typeof_value("exception");
        case fiber::json::JsNodeType::NativeBinary:
        case fiber::json::JsNodeType::HeapBinary:
            return make_typeof_value("binary");
    }
    return make_typeof_value("undefined");
}

VmResult Unaries::iterate(const fiber::json::JsValue &value, ScriptRuntime &runtime) {
    fiber::json::GcHeap *heap = &runtime.heap();
    runtime.maybe_collect();
    fiber::json::GcIterator *iter = nullptr;
    if (value.type_ == fiber::json::JsNodeType::Array) {
        iter = fiber::json::gc_new_array_iterator(heap, reinterpret_cast<fiber::json::GcArray *>(value.gc),
                                                  fiber::json::GcIteratorMode::Values);
    } else if (value.type_ == fiber::json::JsNodeType::Object) {
        iter = fiber::json::gc_new_object_iterator(heap, reinterpret_cast<fiber::json::GcObject *>(value.gc),
                                                   fiber::json::GcIteratorMode::Values);
    } else {
        iter = fiber::json::gc_new_array_iterator(heap, nullptr, fiber::json::GcIteratorMode::Values);
    }
    if (!iter) {
        VmError error;
        error.name = "EXEC_OUT_OF_MEMORY";
        error.message = "out of memory for iterate";
        return std::unexpected(error);
    }
    fiber::json::JsValue out;
    out.type_ = fiber::json::JsNodeType::Interator;
    out.gc = &iter->hdr;
    return out;
}

} // namespace fiber::script::run
