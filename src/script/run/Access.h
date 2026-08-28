#ifndef FIBER_SCRIPT_RUN_ACCESS_H
#define FIBER_SCRIPT_RUN_ACCESS_H

#include <fiber/script/JsGc.h>
#include <fiber/script/ScriptResult.h>

namespace fiber::script::run {

class Access {
public:
    static CallResult expand_object(GcHeap &runtime, ConstValueHandle target, ConstValueHandle addition,
                                    ResultPayload &result) noexcept;
    static CallResult expand_array(GcHeap &runtime, ConstValueHandle target, ConstValueHandle addition,
                                   ResultPayload &result) noexcept;
    static CallResult push_array(GcHeap &runtime, ConstValueHandle target, ConstValueHandle addition,
                                 ResultPayload &result) noexcept;

    static CallResult index_get(GcHeap &runtime, ConstValueHandle parent, ConstValueHandle key,
                                ResultPayload &result) noexcept;
    static CallResult index_set(GcHeap &runtime, ConstValueHandle parent, ConstValueHandle key, ConstValueHandle value,
                                ResultPayload &result) noexcept;
    static CallResult index_set1(GcHeap &runtime, ConstValueHandle parent, ConstValueHandle key, ConstValueHandle value,
                                 ResultPayload &result) noexcept;

    static CallResult prop_get(GcHeap &runtime, ConstValueHandle parent, ConstValueHandle key,
                               ResultPayload &result) noexcept;
    static CallResult prop_set(GcHeap &runtime, ConstValueHandle parent, ConstValueHandle value, ConstValueHandle key,
                               ResultPayload &result) noexcept;
    static CallResult prop_set1(GcHeap &runtime, ConstValueHandle parent, ConstValueHandle value, ConstValueHandle key,
                                ResultPayload &result) noexcept;
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_ACCESS_H
