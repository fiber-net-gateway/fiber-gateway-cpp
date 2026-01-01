#ifndef FIBER_SCRIPT_RUN_BINARIES_H
#define FIBER_SCRIPT_RUN_BINARIES_H

#include "../../common/json/JsGc.h"
#include "VmError.h"

namespace fiber::script::run {

class Binaries {
public:
    static VmResult plus(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);
    static VmResult minus(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);
    static VmResult multiply(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);
    static VmResult divide(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);
    static VmResult modulo(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);

    static VmResult matches(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);
    static VmResult lt(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);
    static VmResult lte(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);
    static VmResult gt(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);
    static VmResult gte(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);
    static VmResult eq(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);
    static VmResult seq(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);
    static VmResult ne(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);
    static VmResult sne(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);
    static VmResult in(const fiber::json::JsValue &a, const fiber::json::JsValue &b, fiber::json::GcHeap *heap);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_BINARIES_H
