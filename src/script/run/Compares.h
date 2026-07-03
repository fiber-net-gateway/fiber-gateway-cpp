#ifndef FIBER_SCRIPT_RUN_COMPARES_H
#define FIBER_SCRIPT_RUN_COMPARES_H

#include "../../common/json/JsGc.h"
#include "../Runtime.h"
#include "../ScriptResult.h"

namespace fiber::script::run {

class Compares {
public:
    static bool neg(ValueHandle value) noexcept;
    static bool logic(ValueHandle value) noexcept;

    static ScriptStatus eq(ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus seq(ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus ne(ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus sne(ValueHandle out, ValueHandle a, ValueHandle b) noexcept;

    static ScriptStatus lt(ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus lte(ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus gt(ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus gte(ValueHandle out, ValueHandle a, ValueHandle b) noexcept;

    static ScriptStatus matches(ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
    static ScriptStatus in(ValueHandle out, ValueHandle a, ValueHandle b) noexcept;
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_COMPARES_H
