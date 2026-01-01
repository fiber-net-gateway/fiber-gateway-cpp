#ifndef FIBER_SCRIPT_RUN_BINARIES_H
#define FIBER_SCRIPT_RUN_BINARIES_H

#include "../../common/json/JsNode.h"

namespace fiber::script::run {

class Binaries {
public:
    static fiber::json::JsValue plus(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static fiber::json::JsValue minus(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static fiber::json::JsValue multiply(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static fiber::json::JsValue divide(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static fiber::json::JsValue modulo(const fiber::json::JsValue &a, const fiber::json::JsValue &b);

    static fiber::json::JsValue matches(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static fiber::json::JsValue lt(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static fiber::json::JsValue lte(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static fiber::json::JsValue gt(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static fiber::json::JsValue gte(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static fiber::json::JsValue eq(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static fiber::json::JsValue seq(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static fiber::json::JsValue ne(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static fiber::json::JsValue sne(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
    static fiber::json::JsValue in(const fiber::json::JsValue &a, const fiber::json::JsValue &b);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_BINARIES_H
