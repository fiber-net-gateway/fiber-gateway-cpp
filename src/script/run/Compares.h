#ifndef FIBER_SCRIPT_RUN_COMPARES_H
#define FIBER_SCRIPT_RUN_COMPARES_H

#include "../../common/json/JsGc.h"
#include "VmError.h"

namespace fiber::script::run {

class Compares {
public:
    static bool neg(const fiber::json::JsValue &value);
    static bool logic(const fiber::json::JsValue &value);

    static VmResult eq(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static VmResult seq(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static VmResult ne(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static VmResult sne(const fiber::json::JsValue &a, const fiber::json::JsValue &b);

    static VmResult lt(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static VmResult lte(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static VmResult gt(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static VmResult gte(const fiber::json::JsValue &a, const fiber::json::JsValue &b);

    static VmResult matches(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static VmResult in(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_COMPARES_H
