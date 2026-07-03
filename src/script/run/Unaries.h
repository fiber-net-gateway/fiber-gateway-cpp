#ifndef FIBER_SCRIPT_RUN_UNARIES_H
#define FIBER_SCRIPT_RUN_UNARIES_H

#include "../../common/json/JsGc.h"
#include "../Runtime.h"
#include "../ScriptResult.h"

namespace fiber::script::run {

class Unaries {
public:
    static ScriptStatus neg(ValueHandle out, ConstValueHandle value) noexcept;
    static ScriptStatus plus(ValueHandle out, ConstValueHandle value) noexcept;
    static ScriptStatus minus(ValueHandle out, ConstValueHandle value) noexcept;
    static ScriptStatus typeof_op(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle value) noexcept;
    static ScriptStatus iterate(ScriptRuntime &runtime, ValueHandle out, ConstValueHandle value) noexcept;
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_UNARIES_H
