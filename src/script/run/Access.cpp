#include "Access.h"

namespace fiber::script::run {

fiber::json::JsValue Access::expand_object(const fiber::json::JsValue &target, const fiber::json::JsValue &addition) {
    (void)target;
    (void)addition;
    return fiber::json::JsValue::make_undefined();
}

fiber::json::JsValue Access::expand_array(const fiber::json::JsValue &target, const fiber::json::JsValue &addition) {
    (void)target;
    (void)addition;
    return fiber::json::JsValue::make_undefined();
}

fiber::json::JsValue Access::push_array(const fiber::json::JsValue &target, const fiber::json::JsValue &addition) {
    (void)target;
    (void)addition;
    return fiber::json::JsValue::make_undefined();
}

fiber::json::JsValue Access::index_get(const fiber::json::JsValue &parent, const fiber::json::JsValue &key) {
    (void)parent;
    (void)key;
    return fiber::json::JsValue::make_undefined();
}

fiber::json::JsValue Access::index_set(const fiber::json::JsValue &parent,
                               const fiber::json::JsValue &key,
                               const fiber::json::JsValue &value) {
    (void)parent;
    (void)key;
    (void)value;
    return fiber::json::JsValue::make_undefined();
}

fiber::json::JsValue Access::index_set1(const fiber::json::JsValue &parent,
                                const fiber::json::JsValue &key,
                                const fiber::json::JsValue &value) {
    (void)parent;
    (void)key;
    (void)value;
    return fiber::json::JsValue::make_undefined();
}

fiber::json::JsValue Access::prop_get(const fiber::json::JsValue &parent, const fiber::json::JsValue &key) {
    (void)parent;
    (void)key;
    return fiber::json::JsValue::make_undefined();
}

fiber::json::JsValue Access::prop_set(const fiber::json::JsValue &parent,
                              const fiber::json::JsValue &value,
                              const fiber::json::JsValue &key) {
    (void)parent;
    (void)value;
    (void)key;
    return fiber::json::JsValue::make_undefined();
}

fiber::json::JsValue Access::prop_set1(const fiber::json::JsValue &parent,
                               const fiber::json::JsValue &value,
                               const fiber::json::JsValue &key) {
    (void)parent;
    (void)value;
    (void)key;
    return fiber::json::JsValue::make_undefined();
}

} // namespace fiber::script::run
