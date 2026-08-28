#ifndef FIBER_SCRIPT_RUN_BINARIES_H
#define FIBER_SCRIPT_RUN_BINARIES_H

#include <fiber/script/JsGc.h>
#include <fiber/script/ScriptResult.h>

namespace fiber::script::run {

class Binaries {
public:
    static CallResult plus(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;
    static CallResult minus(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;
    static CallResult multiply(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;
    static CallResult divide(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;
    static CallResult modulo(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;

    static CallResult matches(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;
    static CallResult lt(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;
    static CallResult lte(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;
    static CallResult gt(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;
    static CallResult gte(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;
    static CallResult eq(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;
    static CallResult seq(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;
    static CallResult ne(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;
    static CallResult sne(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;
    static CallResult in(GcHeap &runtime, ConstValueHandle a, ConstValueHandle b, ResultPayload &result) noexcept;
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_BINARIES_H
