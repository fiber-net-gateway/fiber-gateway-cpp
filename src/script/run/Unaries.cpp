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

ScriptStatus make_typeof_value(ValueHandle out, const char *text) noexcept {
    if (!out) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
    }
    *out = fiber::json::JsValue::make_native_string(const_cast<char *>(text), std::strlen(text));
    return ScriptStatus::success();
}

} // namespace

ScriptStatus Unaries::neg(ValueHandle out, ConstValueHandle value) noexcept {
    return from_js_result(out, fiber::json::js_unary_op(fiber::json::JsUnaryOp::LogicalNot, *value));
}

ScriptStatus Unaries::plus(ValueHandle out, ConstValueHandle value) noexcept {
    return from_js_result(out, fiber::json::js_unary_op(fiber::json::JsUnaryOp::Plus, *value));
}

ScriptStatus Unaries::minus(ValueHandle out, ConstValueHandle value) noexcept {
    return from_js_result(out, fiber::json::js_unary_op(fiber::json::JsUnaryOp::Negate, *value));
}

ScriptStatus Unaries::typeof_op(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle value) noexcept {
    (void) runtime;
    switch (fiber::json::js_value_type(*value)) {
        case fiber::json::JsNodeType::Undefined:
            return make_typeof_value(out, "undefined");
        case fiber::json::JsNodeType::Null:
            return make_typeof_value(out, "null");
        case fiber::json::JsNodeType::Boolean:
            return make_typeof_value(out, "boolean");
        case fiber::json::JsNodeType::Integer:
        case fiber::json::JsNodeType::Float:
            return make_typeof_value(out, "number");
        case fiber::json::JsNodeType::String:
            return make_typeof_value(out, "string");
        case fiber::json::JsNodeType::Array:
            return make_typeof_value(out, "array");
        case fiber::json::JsNodeType::Object:
            return make_typeof_value(out, "object");
        case fiber::json::JsNodeType::Interator:
            return make_typeof_value(out, "iterator");
        case fiber::json::JsNodeType::Exception:
            return make_typeof_value(out, "exception");
        case fiber::json::JsNodeType::Binary:
            return make_typeof_value(out, "binary");
    }
    return make_typeof_value(out, "undefined");
}

ScriptStatus Unaries::iterate(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle value) noexcept {
    fiber::json::GcHeap *heap = &runtime.heap();
    fiber::json::GcIterator *iter = nullptr;
    iter = runtime.alloc_with_gc(fiber::json::gc_estimate_iterator_bytes(), [&]() {
        if (fiber::json::js_value_type(*value) == fiber::json::JsNodeType::Array) {
            return fiber::json::gc_new_array_iterator(
                    heap,
                    const_cast<fiber::json::GcArray *>(
                            fiber::json::js_value_heap_ptr<const fiber::json::GcArray>(*value)),
                    fiber::json::GcIteratorMode::Values);
        }
        if (fiber::json::js_value_type(*value) == fiber::json::JsNodeType::Object) {
            return fiber::json::gc_new_object_iterator(
                    heap,
                    const_cast<fiber::json::GcObject *>(
                            fiber::json::js_value_heap_ptr<const fiber::json::GcObject>(*value)),
                    fiber::json::GcIteratorMode::Values);
        }
        return fiber::json::gc_new_array_iterator(heap, nullptr, fiber::json::GcIteratorMode::Values);
    });
    if (!iter) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
    }
    *out = fiber::json::js_make_heap_ref(&iter->hdr, fiber::json::JsHeapKind::Iterator);
    return ScriptStatus::success();
}

} // namespace fiber::script::run
