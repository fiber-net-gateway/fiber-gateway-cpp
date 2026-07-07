#ifndef FIBER_SCRIPT_RUN_UNARIES_H
#define FIBER_SCRIPT_RUN_UNARIES_H

#include "../Runtime.h"
#include "../ScriptResult.h"
#include "../JsGc.h"

namespace fiber::script::run {

class Unaries {
public:
    static CallResult neg(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept;
    static CallResult plus(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept;
    static CallResult minus(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept;
    static CallResult typeof_op(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept;
    static CallResult iterate(ScriptRuntime &runtime, ConstValueHandle value, ResultPayload &result) noexcept;
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_UNARIES_H
