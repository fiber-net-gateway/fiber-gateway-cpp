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

CallResult make_typeof_value(ResultPayload &result, const char *text) noexcept {
    return set_value(result, fiber::json::JsValue::make_native_string(const_cast<char *>(text), std::strlen(text)));
}

} // namespace

CallResult Unaries::neg(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept {
    (void) runtime;
    return set_value(result, fiber::json::JsValue::make_boolean(!is_truthy(*value)));
}

CallResult Unaries::plus(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept {
    (void) runtime;
    if (!is_numeric_like(fiber::json::js_value_type(*value))) {
        return set_exception(result, fiber::json::ExceptionKind::TypeError);
    }
    if (fiber::json::js_value_type(*value) == fiber::json::JsNodeType::Float) {
        return set_value(result, fiber::json::JsValue::make_float(fiber::json::js_value_double(*value)));
    }
    std::int64_t int_value = 0;
    if (!to_int64(*value, int_value)) {
        return set_exception(result, fiber::json::ExceptionKind::TypeError);
    }
    return set_value(result, fiber::json::JsValue::make_integer(int_value));
}

CallResult Unaries::minus(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept {
    (void) runtime;
    if (!is_numeric_like(fiber::json::js_value_type(*value))) {
        return set_exception(result, fiber::json::ExceptionKind::TypeError);
    }
    if (fiber::json::js_value_type(*value) == fiber::json::JsNodeType::Float) {
        return set_value(result, fiber::json::JsValue::make_float(-fiber::json::js_value_double(*value)));
    }
    std::int64_t int_value = 0;
    if (!to_int64(*value, int_value)) {
        return set_exception(result, fiber::json::ExceptionKind::TypeError);
    }
    if (int_value == std::numeric_limits<std::int64_t>::min()) {
        return set_value(result, fiber::json::JsValue::make_float(-static_cast<double>(int_value)));
    }
    return set_value(result, fiber::json::JsValue::make_integer(-int_value));
}

CallResult Unaries::typeof_op(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept {
    (void) runtime;
    switch (fiber::json::js_value_type(*value)) {
        case fiber::json::JsNodeType::Undefined:
            return make_typeof_value(result, "undefined");
        case fiber::json::JsNodeType::Null:
            return make_typeof_value(result, "null");
        case fiber::json::JsNodeType::Boolean:
            return make_typeof_value(result, "boolean");
        case fiber::json::JsNodeType::Integer:
        case fiber::json::JsNodeType::Float:
            return make_typeof_value(result, "number");
        case fiber::json::JsNodeType::String:
            return make_typeof_value(result, "string");
        case fiber::json::JsNodeType::Array:
            return make_typeof_value(result, "array");
        case fiber::json::JsNodeType::Object:
            return make_typeof_value(result, "object");
        case fiber::json::JsNodeType::Interator:
            return make_typeof_value(result, "iterator");
        case fiber::json::JsNodeType::Exception:
            return make_typeof_value(result, "exception");
        case fiber::json::JsNodeType::Binary:
            return make_typeof_value(result, "binary");
    }
    return make_typeof_value(result, "undefined");
}

CallResult Unaries::iterate(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept {
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
        return set_abort(result, ScriptAbortReason::OutOfMemory);
    }
    return set_value(result, fiber::json::js_make_heap_ref(&iter->hdr, fiber::json::JsHeapKind::Iterator));
}

} // namespace fiber::script::run
