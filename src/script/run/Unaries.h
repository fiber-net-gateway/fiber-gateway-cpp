#ifndef FIBER_SCRIPT_RUN_UNARIES_H
#define FIBER_SCRIPT_RUN_UNARIES_H

#include <fiber/script/JsGc.h>
#include <fiber/script/ScriptResult.h>

namespace fiber::script::run {

class Unaries {
public:
    static CallResult neg(GcHeap &runtime, ConstValueHandle value, ResultPayload &result) noexcept;
    static CallResult plus(GcHeap &runtime, ConstValueHandle value, ResultPayload &result) noexcept;
    static CallResult minus(GcHeap &runtime, ConstValueHandle value, ResultPayload &result) noexcept;
    static CallResult typeof_op(GcHeap &runtime, ConstValueHandle value, ResultPayload &result) noexcept;
    static CallResult iterate(GcHeap &runtime, ConstValueHandle value, ResultPayload &result) noexcept;
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_UNARIES_H
