#ifndef FIBER_SCRIPT_RUN_ACCESS_H
#define FIBER_SCRIPT_RUN_ACCESS_H

#include "../../common/json/JsGc.h"
#include "VmError.h"

namespace fiber::script {
class ScriptRuntime;
}

namespace fiber::script::run {

class Access {
public:
    static VmResult expand_object(const fiber::json::JsValue &target,
                                  const fiber::json::JsValue &addition,
                                  ScriptRuntime &runtime);
    static VmResult expand_array(const fiber::json::JsValue &target,
                                 const fiber::json::JsValue &addition,
                                 ScriptRuntime &runtime);
    static VmResult push_array(const fiber::json::JsValue &target,
                               const fiber::json::JsValue &addition,
                               ScriptRuntime &runtime);

    static VmResult index_get(const fiber::json::JsValue &parent,
                              const fiber::json::JsValue &key,
                              ScriptRuntime &runtime);
    static VmResult index_set(const fiber::json::JsValue &parent,
                              const fiber::json::JsValue &key,
                              const fiber::json::JsValue &value,
                              ScriptRuntime &runtime);
    static VmResult index_set1(const fiber::json::JsValue &parent,
                               const fiber::json::JsValue &key,
                               const fiber::json::JsValue &value,
                               ScriptRuntime &runtime);

    static VmResult prop_get(const fiber::json::JsValue &parent,
                             const fiber::json::JsValue &key,
                             ScriptRuntime &runtime);
    static VmResult prop_set(const fiber::json::JsValue &parent,
                             const fiber::json::JsValue &value,
                             const fiber::json::JsValue &key,
                             ScriptRuntime &runtime);
    static VmResult prop_set1(const fiber::json::JsValue &parent,
                              const fiber::json::JsValue &value,
                              const fiber::json::JsValue &key,
                              ScriptRuntime &runtime);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_ACCESS_H
