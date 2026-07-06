#ifndef FIBER_SCRIPT_RUN_BINARIES_H
#define FIBER_SCRIPT_RUN_BINARIES_H

#include "../../common/json/JsGc.h"
#include "../Runtime.h"
#include "../ScriptResult.h"

namespace fiber::script::run {

class Binaries {
public:
    static CallResult plus(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                           ResultPayload &result) noexcept;
    static CallResult minus(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                            ResultPayload &result) noexcept;
    static CallResult multiply(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                               ResultPayload &result) noexcept;
    static CallResult divide(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                             ResultPayload &result) noexcept;
    static CallResult modulo(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                             ResultPayload &result) noexcept;

    static CallResult matches(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                              ResultPayload &result) noexcept;
    static CallResult lt(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                         ResultPayload &result) noexcept;
    static CallResult lte(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                          ResultPayload &result) noexcept;
    static CallResult gt(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                         ResultPayload &result) noexcept;
    static CallResult gte(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                          ResultPayload &result) noexcept;
    static CallResult eq(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                         ResultPayload &result) noexcept;
    static CallResult seq(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                          ResultPayload &result) noexcept;
    static CallResult ne(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                         ResultPayload &result) noexcept;
    static CallResult sne(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                          ResultPayload &result) noexcept;
    static CallResult in(ScriptRuntime &runtime, ConstValueHandle a, ConstValueHandle b,
                         ResultPayload &result) noexcept;
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_BINARIES_H
