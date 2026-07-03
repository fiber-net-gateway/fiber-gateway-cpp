#include "Unaries.h"

#include "../Runtime.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace fiber::script::run {

namespace {

const fiber::json::GcString *as_heap_string(const fiber::json::JsValue &value) noexcept {
    return fiber::json::js_value_type(value) == fiber::json::JsNodeType::String
                   ? fiber::json::js_value_heap_ptr<const fiber::json::GcString>(value)
                   : nullptr;
}

bool is_numeric_like(fiber::json::JsNodeType type) noexcept {
    return type == fiber::json::JsNodeType::Integer || type == fiber::json::JsNodeType::Float ||
           type == fiber::json::JsNodeType::Boolean || type == fiber::json::JsNodeType::Null;
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

ScriptStatus make_typeof_value(ValueHandle out, const char *text) noexcept {
    if (!out) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
    }
    *out = fiber::json::JsValue::make_native_string(const_cast<char *>(text), std::strlen(text));
    return ScriptStatus::success();
}

} // namespace

ScriptStatus Unaries::neg(ValueHandle out, ConstValueHandle value) noexcept {
    if (!out) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
    }
    *out = fiber::json::JsValue::make_boolean(!is_truthy(*value));
    return ScriptStatus::success();
}

ScriptStatus Unaries::plus(ValueHandle out, ConstValueHandle value) noexcept {
    if (!out) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
    }
    if (!is_numeric_like(fiber::json::js_value_type(*value))) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    if (fiber::json::js_value_type(*value) == fiber::json::JsNodeType::Float) {
        *out = fiber::json::JsValue::make_float(fiber::json::js_value_double(*value));
        return ScriptStatus::success();
    }
    std::int64_t int_value = 0;
    if (!to_int64(*value, int_value)) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    *out = fiber::json::JsValue::make_integer(int_value);
    return ScriptStatus::success();
}

ScriptStatus Unaries::minus(ValueHandle out, ConstValueHandle value) noexcept {
    if (!out) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
    }
    if (!is_numeric_like(fiber::json::js_value_type(*value))) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    if (fiber::json::js_value_type(*value) == fiber::json::JsNodeType::Float) {
        *out = fiber::json::JsValue::make_float(-fiber::json::js_value_double(*value));
        return ScriptStatus::success();
    }
    std::int64_t int_value = 0;
    if (!to_int64(*value, int_value)) {
        return ScriptStatus::abort(ScriptAbortReason::TypeError);
    }
    if (int_value == std::numeric_limits<std::int64_t>::min()) {
        *out = fiber::json::JsValue::make_float(-static_cast<double>(int_value));
        return ScriptStatus::success();
    }
    *out = fiber::json::JsValue::make_integer(-int_value);
    return ScriptStatus::success();
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
