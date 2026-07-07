#ifndef FIBER_SCRIPT_RUN_ACCESS_H
#define FIBER_SCRIPT_RUN_ACCESS_H

#include "../Runtime.h"
#include "../ScriptResult.h"
#include "../json/JsGc.h"

namespace fiber::script::run {

class Access {
public:
    static CallResult expand_object(ScriptRuntime &runtime, ConstValueHandle target, ConstValueHandle addition,
                                    ResultPayload &result) noexcept;
    static CallResult expand_array(ScriptRuntime &runtime, ConstValueHandle target, ConstValueHandle addition,
                                   ResultPayload &result) noexcept;
    static CallResult push_array(ScriptRuntime &runtime, ConstValueHandle target, ConstValueHandle addition,
                                 ResultPayload &result) noexcept;

    static CallResult index_get(ScriptRuntime &runtime, ConstValueHandle parent, ConstValueHandle key,
                                ResultPayload &result) noexcept;
    static CallResult index_set(ScriptRuntime &runtime, ConstValueHandle parent, ConstValueHandle key,
                                ConstValueHandle value, ResultPayload &result) noexcept;
    static CallResult index_set1(ScriptRuntime &runtime, ConstValueHandle parent, ConstValueHandle key,
                                 ConstValueHandle value, ResultPayload &result) noexcept;

    static CallResult prop_get(ScriptRuntime &runtime, ConstValueHandle parent, ConstValueHandle key,
                               ResultPayload &result) noexcept;
    static CallResult prop_set(ScriptRuntime &runtime, ConstValueHandle parent, ConstValueHandle value,
                               ConstValueHandle key, ResultPayload &result) noexcept;
    static CallResult prop_set1(ScriptRuntime &runtime, ConstValueHandle parent, ConstValueHandle value,
                                ConstValueHandle key, ResultPayload &result) noexcept;
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_ACCESS_H
