#ifndef FIBER_SCRIPT_RUN_BINARIES_H
#define FIBER_SCRIPT_RUN_BINARIES_H

#include "../../common/json/JsGc.h"
#include "VmError.h"

namespace fiber::script {
class ScriptRuntime;
}

namespace fiber::script::run {

class Binaries {
public:
    static VmResult plus(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static VmResult minus(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static VmResult multiply(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static VmResult divide(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static VmResult modulo(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);

    static VmResult matches(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static VmResult lt(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static VmResult lte(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static VmResult gt(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static VmResult gte(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static VmResult eq(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static VmResult seq(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static VmResult ne(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static VmResult sne(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static VmResult in(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_BINARIES_H
