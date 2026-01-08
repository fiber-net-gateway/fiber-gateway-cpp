#ifndef FIBER_SCRIPT_EXECUTION_CONTEXT_H
#define FIBER_SCRIPT_EXECUTION_CONTEXT_H

#include <cstddef>

#include "../common/json/JsNode.h"

namespace fiber::script {

class ScriptRuntime;

class ExecutionContext {
public:
    virtual ~ExecutionContext() = default;

    virtual ScriptRuntime &runtime() = 0;
    virtual const fiber::json::JsValue &root() const = 0;
    virtual void *attach() const = 0;
    virtual const fiber::json::JsValue &arg_value(std::size_t index) const = 0;
    virtual std::size_t arg_count() const = 0;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_EXECUTION_CONTEXT_H
