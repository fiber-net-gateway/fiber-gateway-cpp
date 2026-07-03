#ifndef FIBER_SCRIPT_RUN_ACCESS_H
#define FIBER_SCRIPT_RUN_ACCESS_H

#include "../../common/json/JsGc.h"
#include "../Runtime.h"
#include "../ScriptResult.h"

namespace fiber::script::run {

class Access {
public:
    static ScriptStatus expand_object(ScriptRuntime &runtime, ValueHandle out, ValueHandle target,
                                      ValueHandle addition) noexcept;
    static ScriptStatus expand_array(ScriptRuntime &runtime, ValueHandle out, ValueHandle target,
                                     ValueHandle addition) noexcept;
    static ScriptStatus push_array(ScriptRuntime &runtime, ValueHandle out, ValueHandle target,
                                   ValueHandle addition) noexcept;

    static ScriptStatus index_get(ScriptRuntime &runtime, ValueHandle out, ValueHandle parent,
                                  ValueHandle key) noexcept;
    static ScriptStatus index_set(ScriptRuntime &runtime, ValueHandle out, ValueHandle parent, ValueHandle key,
                                  ValueHandle value) noexcept;
    static ScriptStatus index_set1(ScriptRuntime &runtime, ValueHandle out, ValueHandle parent, ValueHandle key,
                                   ValueHandle value) noexcept;

    static ScriptStatus prop_get(ScriptRuntime &runtime, ValueHandle out, ValueHandle parent, ValueHandle key) noexcept;
    static ScriptStatus prop_set(ScriptRuntime &runtime, ValueHandle out, ValueHandle parent, ValueHandle value,
                                 ValueHandle key) noexcept;
    static ScriptStatus prop_set1(ScriptRuntime &runtime, ValueHandle out, ValueHandle parent, ValueHandle value,
                                  ValueHandle key) noexcept;
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_ACCESS_H
