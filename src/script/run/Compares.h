#ifndef FIBER_SCRIPT_RUN_COMPARES_H
#define FIBER_SCRIPT_RUN_COMPARES_H

#include "../../common/json/JsGc.h"
#include "../Runtime.h"
#include "../ScriptResult.h"

namespace fiber::script::run {

class Compares {
public:
    static bool neg(ConstValueHandle value) noexcept;
    static bool logic(ConstValueHandle value) noexcept;

    static ScriptStatus eq(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept;
    static ScriptStatus seq(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept;
    static ScriptStatus ne(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept;
    static ScriptStatus sne(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept;

    static ScriptStatus lt(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept;
    static ScriptStatus lte(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept;
    static ScriptStatus gt(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept;
    static ScriptStatus gte(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept;

    static ScriptStatus matches(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept;
    static ScriptStatus in(ValueHandle out, ConstValueHandle a, ConstValueHandle b) noexcept;
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_COMPARES_H
