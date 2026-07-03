#ifndef FIBER_SCRIPT_RUN_BINARIES_H
#define FIBER_SCRIPT_RUN_BINARIES_H

#include "../../common/json/JsGc.h"
#include "../Runtime.h"
#include "../ScriptResult.h"

namespace fiber::script::run {

class Binaries {
public:
    static ScriptStatus plus(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus minus(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus multiply(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus divide(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus modulo(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;

    static ScriptStatus matches(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus lt(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus lte(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus gt(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus gte(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus eq(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus seq(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus ne(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus sne(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus in(ScriptRuntime &runtime, ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_BINARIES_H
