#include "Compares.h"

#include "../../common/json/JsValueOps.h"

#include <string>
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

VmResult make_bool(bool value) {
    return fiber::json::JsValue::make_boolean(value);
}

} // namespace

bool Compares::neg(const fiber::json::JsValue &value) {
    return !logic(value);
}

bool Compares::logic(const fiber::json::JsValue &value) {
    fiber::json::JsOpResult result = fiber::json::js_unary_op(fiber::json::JsUnaryOp::LogicalNot, value);
    if (result.error != fiber::json::JsOpError::None) {
        return false;
    }
    return !result.value.b;
}

VmResult Compares::eq(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Eq, a, b, nullptr), "==");
}

VmResult Compares::seq(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::StrictEq, a, b, nullptr), "===");
}

VmResult Compares::ne(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Ne, a, b, nullptr), "!=");
}

VmResult Compares::sne(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::StrictNe, a, b, nullptr), "!==");
}

VmResult Compares::lt(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Lt, a, b, nullptr), "<");
}

VmResult Compares::lte(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Le, a, b, nullptr), "<=");
}

VmResult Compares::gt(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Gt, a, b, nullptr), ">");
}

VmResult Compares::gte(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return from_js_result(fiber::json::js_binary_op(fiber::json::JsBinaryOp::Ge, a, b, nullptr), ">=");
}

VmResult Compares::matches(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    (void)a;
    (void)b;
    return make_bool(false);
}

VmResult Compares::in(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    if (b.type_ == fiber::json::JsNodeType::Array) {
        if (a.type_ != fiber::json::JsNodeType::Integer) {
            return make_bool(false);
        }
        auto *arr = reinterpret_cast<const fiber::json::GcArray *>(b.gc);
        if (!arr || a.i < 0) {
            return make_bool(false);
        }
        return make_bool(static_cast<std::size_t>(a.i) < arr->size);
    }
    if (b.type_ == fiber::json::JsNodeType::Object) {
        auto *obj = reinterpret_cast<const fiber::json::GcObject *>(b.gc);
        if (!obj) {
            return make_bool(false);
        }
        if (a.type_ == fiber::json::JsNodeType::HeapString) {
            auto *key_str = reinterpret_cast<const fiber::json::GcString *>(a.gc);
            const fiber::json::JsValue *found = fiber::json::gc_object_get(obj, key_str);
            return make_bool(found != nullptr);
        }
        if (a.type_ == fiber::json::JsNodeType::NativeString) {
            std::string key(a.ns.data, a.ns.len);
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
