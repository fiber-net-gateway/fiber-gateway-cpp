#include "Compares.h"

#include "../../common/json/JsValueOps.h"

#include <string>
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

ScriptResult make_bool(bool value) { return fiber::json::JsValue::make_boolean(value); }

} // namespace

bool Compares::neg(const fiber::json::JsValue &value) { return !logic(value); }

bool Compares::logic(const fiber::json::JsValue &value) {
    fiber::json::JsOpResult result = fiber::json::js_unary_op(fiber::json::JsUnaryOp::LogicalNot, value);
    if (result.error != fiber::json::JsOpError::None) {
        return false;
    }
    return !fiber::json::js_value_bool(result.value);
}

ScriptResult Compares::eq(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Eq, a, b, nullptr), "==");
}

ScriptResult Compares::seq(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::StrictEq, a, b, nullptr), "===");
}

ScriptResult Compares::ne(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Ne, a, b, nullptr), "!=");
}

ScriptResult Compares::sne(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::StrictNe, a, b, nullptr), "!==");
}

ScriptResult Compares::lt(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Lt, a, b, nullptr), "<");
}

ScriptResult Compares::lte(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Le, a, b, nullptr), "<=");
}

ScriptResult Compares::gt(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Gt, a, b, nullptr), ">");
}

ScriptResult Compares::gte(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Ge, a, b, nullptr), ">=");
}

ScriptResult Compares::matches(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    (void) a;
    (void) b;
    return make_bool(false);
}

ScriptResult Compares::in(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    if (fiber::json::js_value_type(b) == fiber::json::JsNodeType::Array) {
        if (fiber::json::js_value_type(a) != fiber::json::JsNodeType::Integer) {
            return make_bool(false);
        }
        auto *arr = fiber::json::js_value_heap_ptr<const fiber::json::GcArray>(b);
        std::int64_t index = fiber::json::js_value_int64(a);
        if (!arr || index < 0) {
            return make_bool(false);
        }
        return make_bool(static_cast<std::size_t>(index) < arr->size);
    }
    if (fiber::json::js_value_type(b) == fiber::json::JsNodeType::Object) {
        auto *obj = fiber::json::js_value_heap_ptr<const fiber::json::GcObject>(b);
        if (!obj) {
            return make_bool(false);
        }
        if (fiber::json::js_value_type(a) == fiber::json::JsNodeType::String &&
            !fiber::json::js_value_is_borrowed_string(a)) {
            auto *key_str = fiber::json::js_value_heap_ptr<const fiber::json::GcString>(a);
            const fiber::json::JsValue *found = fiber::json::gc_object_get(obj, key_str);
            return make_bool(found != nullptr);
        }
        if (fiber::json::js_value_type(a) == fiber::json::JsNodeType::String &&
            fiber::json::js_value_is_borrowed_string(a)) {
            fiber::json::NativeStr native = fiber::json::js_value_native_string(a);
            std::string key(native.data, native.len);
            for (std::size_t i = 0; i < obj->size; ++i) {
                const fiber::json::GcObjectEntry *entry = fiber::json::gc_object_entry_at(obj, i);
                if (!entry || !entry->occupied || !entry->key) {
                    continue;
                }
                std::string entry_key;
                if (fiber::json::gc_string_to_utf8(entry->key, entry_key) && entry_key == key) {
                    return make_bool(true);
                }
            }
        }
        return make_bool(false);
    }
    return make_bool(false);
}

} // namespace fiber::script::run
