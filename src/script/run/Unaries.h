#ifndef FIBER_SCRIPT_RUN_UNARIES_H
#define FIBER_SCRIPT_RUN_UNARIES_H

#include "../../common/json/JsGc.h"
#include "../ScriptResult.h"

namespace fiber::script {
class ScriptRuntime;
}

namespace fiber::script::run {

class Unaries {
public:
    static ScriptResult neg(const fiber::json::JsValue &value);
    static ScriptResult plus(const fiber::json::JsValue &value);
    static ScriptResult minus(const fiber::json::JsValue &value);
    static ScriptResult typeof_op(const fiber::json::JsValue &value, ScriptRuntime &runtime);
    static ScriptResult iterate(const fiber::json::JsValue &value, ScriptRuntime &runtime);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_UNARIES_H
