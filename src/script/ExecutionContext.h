#ifndef FIBER_SCRIPT_EXECUTION_CONTEXT_H
#define FIBER_SCRIPT_EXECUTION_CONTEXT_H

#include <cstddef>

#include "JsValue.h"

namespace fiber::script {

class GcHeap;

class ExecutionContext {
public:
    virtual ~ExecutionContext() = default;

    virtual GcHeap &runtime() = 0;
    virtual const fiber::script::JsValue &root() const = 0;
    virtual void *attach() const = 0;
    virtual const fiber::script::JsValue &arg_value(std::size_t index) const = 0;
    virtual std::size_t arg_count() const = 0;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_EXECUTION_CONTEXT_H
