#include "Compares.h"

#include "../../common/json/JsValueOps.h"

#include <string>

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

ScriptStatus make_bool(ValueHandle out, bool value) noexcept {
    if (!out) {
        return ScriptStatus::abort(ScriptAbortReason::OutOfMemory);
    }
    *out = fiber::json::JsValue::make_boolean(value);
    return ScriptStatus::success();
}

} // namespace

bool Compares::neg(ValueHandle value) noexcept { return !logic(value); }

bool Compares::logic(ValueHandle value) noexcept {
    if (!value) {
        return false;
    }
    fiber::json::JsOpResult result = fiber::json::js_unary_op(fiber::json::JsUnaryOp::LogicalNot, *value);
    if (result.error != fiber::json::JsOpError::None) {
        return false;
    }
    return !fiber::json::js_value_bool(result.value);
}

ScriptStatus Compares::eq(ValueHandle out, ValueHandle a, ValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Eq, *a, *b, nullptr));
}

ScriptStatus Compares::seq(ValueHandle out, ValueHandle a, ValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::StrictEq, *a, *b, nullptr));
}

ScriptStatus Compares::ne(ValueHandle out, ValueHandle a, ValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Ne, *a, *b, nullptr));
}

ScriptStatus Compares::sne(ValueHandle out, ValueHandle a, ValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::StrictNe, *a, *b, nullptr));
}

ScriptStatus Compares::lt(ValueHandle out, ValueHandle a, ValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Lt, *a, *b, nullptr));
}

ScriptStatus Compares::lte(ValueHandle out, ValueHandle a, ValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Le, *a, *b, nullptr));
}

ScriptStatus Compares::gt(ValueHandle out, ValueHandle a, ValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Gt, *a, *b, nullptr));
}

ScriptStatus Compares::gte(ValueHandle out, ValueHandle a, ValueHandle b) noexcept {
    return from_js_result(out, fiber::json::js_binary_op(fiber::json::JsBinaryOp::Ge, *a, *b, nullptr));
}

ScriptStatus Compares::matches(ValueHandle out, ValueHandle a, ValueHandle b) noexcept {
    (void) a;
    (void) b;
    return make_bool(out, false);
}

ScriptStatus Compares::in(ValueHandle out, ValueHandle a, ValueHandle b) noexcept {
    if (!a || !b) {
        return make_bool(out, false);
    }
    if (fiber::json::js_value_type(*b) == fiber::json::JsNodeType::Array) {
        if (fiber::json::js_value_type(*a) != fiber::json::JsNodeType::Integer) {
            return make_bool(out, false);
        }
        auto *arr = fiber::json::js_value_heap_ptr<const fiber::json::GcArray>(*b);
        std::int64_t index = fiber::json::js_value_int64(*a);
        if (!arr || index < 0) {
            return make_bool(out, false);
        }
        return make_bool(out, static_cast<std::size_t>(index) < arr->size);
    }
    if (fiber::json::js_value_type(*b) == fiber::json::JsNodeType::Object) {
        auto *obj = fiber::json::js_value_heap_ptr<const fiber::json::GcObject>(*b);
        if (!obj) {
            return make_bool(out, false);
        }
        if (fiber::json::js_value_type(*a) == fiber::json::JsNodeType::String &&
            !fiber::json::js_value_is_borrowed_string(*a)) {
            auto *key_str = fiber::json::js_value_heap_ptr<const fiber::json::GcString>(*a);
            const fiber::json::JsValue *found = fiber::json::gc_object_get(obj, key_str);
            return make_bool(out, found != nullptr);
        }
        if (fiber::json::js_value_type(*a) == fiber::json::JsNodeType::String &&
            fiber::json::js_value_is_borrowed_string(*a)) {
            fiber::json::NativeStr native = fiber::json::js_value_native_string(*a);
            std::string key(native.data, native.len);
            for (std::size_t i = 0; i < obj->size; ++i) {
                const fiber::json::GcObjectEntry *entry = fiber::json::gc_object_entry_at(obj, i);
                if (!entry || !entry->occupied || !entry->key) {
                    continue;
                }
                std::string entry_key;
                if (fiber::json::gc_string_to_utf8(entry->key, entry_key) && entry_key == key) {
                    return make_bool(out, true);
                }
            }
        }
        return make_bool(out, false);
    }
    return make_bool(out, false);
}

} // namespace fiber::script::run
