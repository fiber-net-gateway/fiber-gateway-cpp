#ifndef FIBER_SCRIPT_RUN_UNARIES_H
#define FIBER_SCRIPT_RUN_UNARIES_H

#include "../../common/json/JsGc.h"
#include "VmError.h"

namespace fiber::script {
class ScriptRuntime;
}

namespace fiber::script::run {

class Unaries {
public:
    static VmResult neg(const fiber::json::JsValue &value);
    static VmResult plus(const fiber::json::JsValue &value);
    static VmResult minus(const fiber::json::JsValue &value);
    static VmResult typeof_op(const fiber::json::JsValue &value, ScriptRuntime &runtime);
    static VmResult iterate(const fiber::json::JsValue &value, ScriptRuntime &runtime);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_UNARIES_H
