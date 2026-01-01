#ifndef FIBER_SCRIPT_ASYNC_EXECUTION_CONTEXT_H
#define FIBER_SCRIPT_ASYNC_EXECUTION_CONTEXT_H

#include "../ExecutionContext.h"

namespace fiber::script {

class ScriptRuntime;

class AsyncExecutionContext : public ExecutionContext {
public:
    ~AsyncExecutionContext() override = default;

    virtual ScriptRuntime &runtime() = 0;
    virtual void return_value(const fiber::json::JsValue &value) = 0;
    virtual void throw_value(const fiber::json::JsValue &value) = 0;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_ASYNC_EXECUTION_CONTEXT_H
