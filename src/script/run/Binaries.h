#ifndef FIBER_SCRIPT_RUN_BINARIES_H
#define FIBER_SCRIPT_RUN_BINARIES_H

#include "../../common/json/JsGc.h"
#include "../ScriptResult.h"

namespace fiber::script {
class ScriptRuntime;
}

namespace fiber::script::run {

class Binaries {
public:
    static ScriptResult plus(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static ScriptResult minus(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static ScriptResult multiply(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static ScriptResult divide(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static ScriptResult modulo(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);

    static ScriptResult matches(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static ScriptResult lt(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static ScriptResult lte(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static ScriptResult gt(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static ScriptResult gte(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static ScriptResult eq(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static ScriptResult seq(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static ScriptResult ne(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static ScriptResult sne(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
    static ScriptResult in(const fiber::json::JsValue &a, const fiber::json::JsValue &b, ScriptRuntime &runtime);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_BINARIES_H
