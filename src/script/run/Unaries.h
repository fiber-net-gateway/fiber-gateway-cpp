#ifndef FIBER_SCRIPT_RUN_UNARIES_H
#define FIBER_SCRIPT_RUN_UNARIES_H

#include "../../common/json/JsNode.h"

namespace fiber::script::run {

class Unaries {
public:
    static fiber::json::JsValue neg(const fiber::json::JsValue &value);
    static fiber::json::JsValue plus(const fiber::json::JsValue &value);
    static fiber::json::JsValue minus(const fiber::json::JsValue &value);
    static fiber::json::JsValue typeof_op(const fiber::json::JsValue &value);
    static fiber::json::JsValue iterate(const fiber::json::JsValue &value);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_UNARIES_H
