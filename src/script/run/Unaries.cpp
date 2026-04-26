#include "Unaries.h"

#include "../../common/json/JsValueOps.h"
#include "../Runtime.h"

#include <cstring>
#include <string_view>

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

ScriptResult from_js_result(const fiber::json::JsOpResult &result, std::string_view op) {
    (void) op;
    if (result.error == fiber::json::JsOpError::None) {
        return result.value;
    }
    return ScriptResult::abort(map_error(result.error));
}

ScriptResult make_typeof_value(const char *text) {
    fiber::json::JsValue value = fiber::json::JsValue::make_native_string(const_cast<char *>(text), std::strlen(text));
    return value;
}

} // namespace

ScriptResult Unaries::neg(const fiber::json::JsValue &value) {
    return from_js_result(fiber::json::js_unary_op(fiber::json::JsUnaryOp::LogicalNot, value), "!");
}

ScriptResult Unaries::plus(const fiber::json::JsValue &value) {
    return from_js_result(fiber::json::js_unary_op(fiber::json::JsUnaryOp::Plus, value), "+");
}

ScriptResult Unaries::minus(const fiber::json::JsValue &value) {
    return from_js_result(fiber::json::js_unary_op(fiber::json::JsUnaryOp::Negate, value), "-");
}

ScriptResult Unaries::typeof_op(const fiber::json::JsValue &value, ScriptRuntime &runtime) {
    (void) runtime;
    switch (fiber::json::js_value_type(value)) {
        case fiber::json::JsNodeType::Undefined:
            return make_typeof_value("undefined");
        case fiber::json::JsNodeType::Null:
            return make_typeof_value("null");
        case fiber::json::JsNodeType::Boolean:
            return make_typeof_value("boolean");
        case fiber::json::JsNodeType::Integer:
        case fiber::json::JsNodeType::Float:
            return make_typeof_value("number");
        case fiber::json::JsNodeType::String:
            return make_typeof_value("string");
        case fiber::json::JsNodeType::Array:
            return make_typeof_value("array");
        case fiber::json::JsNodeType::Object:
            return make_typeof_value("object");
        case fiber::json::JsNodeType::Interator:
            return make_typeof_value("iterator");
        case fiber::json::JsNodeType::Exception:
            return make_typeof_value("exception");
        case fiber::json::JsNodeType::Binary:
            return make_typeof_value("binary");
    }
    return make_typeof_value("undefined");
}

ScriptResult Unaries::iterate(const fiber::json::JsValue &value, ScriptRuntime &runtime) {
    fiber::json::GcHeap *heap = &runtime.heap();
    fiber::json::GcIterator *iter = nullptr;
    iter = runtime.alloc_with_gc(fiber::json::gc_estimate_iterator_bytes(), [&]() {
        if (fiber::json::js_value_type(value) == fiber::json::JsNodeType::Array) {
            return fiber::json::gc_new_array_iterator(
                    heap,
                    fiber::json::js_value_heap_ptr<fiber::json::GcArray>(const_cast<fiber::json::JsValue &>(value)),
                    fiber::json::GcIteratorMode::Values);
        }
        if (fiber::json::js_value_type(value) == fiber::json::JsNodeType::Object) {
            return fiber::json::gc_new_object_iterator(
                    heap,
                    fiber::json::js_value_heap_ptr<fiber::json::GcObject>(const_cast<fiber::json::JsValue &>(value)),
                    fiber::json::GcIteratorMode::Values);
        }
        return fiber::json::gc_new_array_iterator(heap, nullptr, fiber::json::GcIteratorMode::Values);
    });
    if (!iter) {
        return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return fiber::json::js_make_heap_ref(&iter->hdr, fiber::json::JsHeapKind::Iterator);
}

} // namespace fiber::script::run
