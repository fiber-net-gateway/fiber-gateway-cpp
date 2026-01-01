#ifndef FIBER_SCRIPT_RUN_COMPARES_H
#define FIBER_SCRIPT_RUN_COMPARES_H

#include "../../common/json/JsNode.h"

namespace fiber::script::run {

class Compares {
public:
    static bool neg(const fiber::json::JsValue &value);
    static bool logic(const fiber::json::JsValue &value);

    static bool eq(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static bool seq(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static bool ne(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static bool sne(const fiber::json::JsValue &a, const fiber::json::JsValue &b);

    static bool lt(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static bool lte(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static bool gt(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static bool gte(const fiber::json::JsValue &a, const fiber::json::JsValue &b);

    static bool matches(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static bool in(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_COMPARES_H
