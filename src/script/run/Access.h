#ifndef FIBER_SCRIPT_RUN_ACCESS_H
#define FIBER_SCRIPT_RUN_ACCESS_H

#include "../../common/json/JsGc.h"
#include "VmError.h"

namespace fiber::script::run {

class Access {
public:
    static VmResult expand_object(const fiber::json::JsValue &target,
                                  const fiber::json::JsValue &addition,
                                  fiber::json::GcHeap *heap);
    static VmResult expand_array(const fiber::json::JsValue &target,
                                 const fiber::json::JsValue &addition,
                                 fiber::json::GcHeap *heap);
    static VmResult push_array(const fiber::json::JsValue &target,
                               const fiber::json::JsValue &addition,
                               fiber::json::GcHeap *heap);

    static VmResult index_get(const fiber::json::JsValue &parent,
                              const fiber::json::JsValue &key,
                              fiber::json::GcHeap *heap);
    static VmResult index_set(const fiber::json::JsValue &parent,
                              const fiber::json::JsValue &key,
                              const fiber::json::JsValue &value,
                              fiber::json::GcHeap *heap);
    static VmResult index_set1(const fiber::json::JsValue &parent,
                               const fiber::json::JsValue &key,
                               const fiber::json::JsValue &value,
                               fiber::json::GcHeap *heap);

    static VmResult prop_get(const fiber::json::JsValue &parent,
                             const fiber::json::JsValue &key,
                             fiber::json::GcHeap *heap);
    static VmResult prop_set(const fiber::json::JsValue &parent,
                             const fiber::json::JsValue &value,
                             const fiber::json::JsValue &key,
                             fiber::json::GcHeap *heap);
    static VmResult prop_set1(const fiber::json::JsValue &parent,
                              const fiber::json::JsValue &value,
                              const fiber::json::JsValue &key,
                              fiber::json::GcHeap *heap);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_ACCESS_H
