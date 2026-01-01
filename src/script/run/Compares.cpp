#include "Compares.h"

namespace fiber::script::run {

bool Compares::neg(const fiber::json::JsValue &value) {
    return !logic(value);
}

bool Compares::logic(const fiber::json::JsValue &value) {
    (void)value;
    return false;
}

bool Compares::eq(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    (void)a;
    (void)b;
    return false;
}

bool Compares::seq(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    (void)a;
    (void)b;
    return false;
}

bool Compares::ne(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return !eq(a, b);
}

bool Compares::sne(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return !seq(a, b);
}

bool Compares::lt(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    (void)a;
    (void)b;
    return false;
}

bool Compares::lte(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    return !gt(a, b);
}

bool Compares::gt(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    (void)a;
    (void)b;
    return false;
}

bool Compares::gte(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    (void)a;
    (void)b;
    return false;
}

bool Compares::matches(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    (void)a;
    (void)b;
    return false;
}

bool Compares::in(const fiber::json::JsValue &a, const fiber::json::JsValue &b) {
    (void)a;
    (void)b;
    return false;
}

} // namespace fiber::script::run
