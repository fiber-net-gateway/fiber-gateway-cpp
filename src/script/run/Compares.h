#ifndef FIBER_SCRIPT_RUN_COMPARES_H
#define FIBER_SCRIPT_RUN_COMPARES_H

#include "../../common/json/JsGc.h"
#include "../ScriptResult.h"

namespace fiber::script::run {

class Compares {
public:
    static bool neg(const fiber::json::JsValue &value);
    static bool logic(const fiber::json::JsValue &value);

    static ScriptResult eq(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static ScriptResult seq(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static ScriptResult ne(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static ScriptResult sne(const fiber::json::JsValue &a, const fiber::json::JsValue &b);

    static ScriptResult lt(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static ScriptResult lte(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static ScriptResult gt(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static ScriptResult gte(const fiber::json::JsValue &a, const fiber::json::JsValue &b);

    static ScriptResult matches(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static ScriptResult in(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_COMPARES_H
