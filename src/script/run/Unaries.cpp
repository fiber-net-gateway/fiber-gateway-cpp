#include "Unaries.h"

namespace fiber::script::run {

fiber::json::JsValue Unaries::neg(const fiber::json::JsValue &value) {
    (void)value;
    return fiber::json::JsValue::make_undefined();
}

fiber::json::JsValue Unaries::plus(const fiber::json::JsValue &value) {
    (void)value;
    return fiber::json::JsValue::make_undefined();
}

fiber::json::JsValue Unaries::minus(const fiber::json::JsValue &value) {
    (void)value;
    return fiber::json::JsValue::make_undefined();
}

fiber::json::JsValue Unaries::typeof_op(const fiber::json::JsValue &value) {
    (void)value;
    return fiber::json::JsValue::make_undefined();
}

fiber::json::JsValue Unaries::iterate(const fiber::json::JsValue &value) {
    (void)value;
    return fiber::json::JsValue::make_undefined();
}

} // namespace fiber::script::run
