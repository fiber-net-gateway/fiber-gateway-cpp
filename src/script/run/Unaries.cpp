#include "Unaries.h"

#include "../Runtime.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace fiber::script::run {

namespace {

const fiber::script::GcString *as_heap_string(const fiber::script::JsValue &value) noexcept {
    return fiber::script::js_value_type(value) == fiber::script::JsNodeType::String
                   ? fiber::script::js_value_heap_ptr<const fiber::script::GcString>(value)
                   : nullptr;
}

bool is_numeric_like(fiber::script::JsNodeType type) noexcept {
    return type == fiber::script::JsNodeType::Integer || type == fiber::script::JsNodeType::Float ||
           type == fiber::script::JsNodeType::Boolean || type == fiber::script::JsNodeType::Null;
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

bool is_truthy(const fiber::script::JsValue &value) noexcept {
    switch (fiber::script::js_value_type(value)) {
        case fiber::script::JsNodeType::Undefined:
        case fiber::script::JsNodeType::Null:
            return false;
        case fiber::script::JsNodeType::Boolean:
            return fiber::script::js_value_bool(value);
        case fiber::script::JsNodeType::Integer:
            return fiber::script::js_value_int64(value) != 0;
        case fiber::script::JsNodeType::Float:
            return fiber::script::js_value_double(value) != 0.0 && !std::isnan(fiber::script::js_value_double(value));
        case fiber::script::JsNodeType::String:
            if (fiber::script::js_value_is_borrowed_string(value)) {
                return fiber::script::js_value_native_string(value).len > 0;
            }
            if (auto *str = as_heap_string(value)) {
                return str->len > 0;
            }
            return false;
        case fiber::script::JsNodeType::Binary:
        case fiber::script::JsNodeType::Array:
        case fiber::script::JsNodeType::Object:
        case fiber::script::JsNodeType::Interator:
        case fiber::script::JsNodeType::Exception:
            return true;
    }
    return false;
}

CallResult make_typeof_value(ResultPayload &result, const char *text) noexcept {
    return set_value(result, fiber::script::JsValue::make_native_string(const_cast<char *>(text), std::strlen(text)));
}

} // namespace

CallResult Unaries::neg(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept {
    (void) runtime;
    return set_value(result, fiber::script::JsValue::make_boolean(!is_truthy(*value)));
}

CallResult Unaries::plus(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept {
    (void) runtime;
    if (!is_numeric_like(fiber::script::js_value_type(*value))) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (fiber::script::js_value_type(*value) == fiber::script::JsNodeType::Float) {
        return set_value(result, fiber::script::JsValue::make_float(fiber::script::js_value_double(*value)));
    }
    std::int64_t int_value = 0;
    if (!to_int64(*value, int_value)) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    return set_value(result, fiber::script::JsValue::make_integer(int_value));
}

CallResult Unaries::minus(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept {
    (void) runtime;
    if (!is_numeric_like(fiber::script::js_value_type(*value))) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (fiber::script::js_value_type(*value) == fiber::script::JsNodeType::Float) {
        return set_value(result, fiber::script::JsValue::make_float(-fiber::script::js_value_double(*value)));
    }
    std::int64_t int_value = 0;
    if (!to_int64(*value, int_value)) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (int_value == std::numeric_limits<std::int64_t>::min()) {
        return set_value(result, fiber::script::JsValue::make_float(-static_cast<double>(int_value)));
    }
    return set_value(result, fiber::script::JsValue::make_integer(-int_value));
}

CallResult Unaries::typeof_op(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept {
    (void) runtime;
    switch (fiber::script::js_value_type(*value)) {
        case fiber::script::JsNodeType::Undefined:
            return make_typeof_value(result, "undefined");
        case fiber::script::JsNodeType::Null:
            return make_typeof_value(result, "null");
        case fiber::script::JsNodeType::Boolean:
            return make_typeof_value(result, "boolean");
        case fiber::script::JsNodeType::Integer:
        case fiber::script::JsNodeType::Float:
            return make_typeof_value(result, "number");
        case fiber::script::JsNodeType::String:
            return make_typeof_value(result, "string");
        case fiber::script::JsNodeType::Array:
            return make_typeof_value(result, "array");
        case fiber::script::JsNodeType::Object:
            return make_typeof_value(result, "object");
        case fiber::script::JsNodeType::Interator:
            return make_typeof_value(result, "iterator");
        case fiber::script::JsNodeType::Exception:
            return make_typeof_value(result, "exception");
        case fiber::script::JsNodeType::Binary:
            return make_typeof_value(result, "binary");
    }
    return make_typeof_value(result, "undefined");
}

CallResult Unaries::iterate(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept {
    fiber::script::GcHeap *heap = &runtime.heap();
    fiber::script::GcIterator *iter = nullptr;
    iter = runtime.alloc_with_gc(fiber::script::gc_estimate_iterator_bytes(), [&]() {
        if (fiber::script::js_value_type(*value) == fiber::script::JsNodeType::Array) {
            return fiber::script::gc_new_array_iterator(
                    heap,
                    const_cast<fiber::script::GcArray *>(
                            fiber::script::js_value_heap_ptr<const fiber::script::GcArray>(*value)),
                    fiber::script::GcIteratorMode::Values);
        }
        if (fiber::script::js_value_type(*value) == fiber::script::JsNodeType::Object) {
            return fiber::script::gc_new_object_iterator(
                    heap,
                    const_cast<fiber::script::GcObject *>(
                            fiber::script::js_value_heap_ptr<const fiber::script::GcObject>(*value)),
                    fiber::script::GcIteratorMode::Values);
        }
        return fiber::script::gc_new_array_iterator(heap, nullptr, fiber::script::GcIteratorMode::Values);
    });
    if (!iter) {
        return set_abort(result, ScriptAbortReason::OutOfMemory);
    }
    return set_value(result, fiber::script::js_make_heap_ref(&iter->hdr, fiber::script::JsHeapKind::Iterator));
}

} // namespace fiber::script::run
