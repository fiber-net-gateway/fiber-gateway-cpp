#ifndef FIBER_SCRIPT_RUN_INTERPRETER_VM_H
#define FIBER_SCRIPT_RUN_INTERPRETER_VM_H

#include <vector>

#include "../ExecutionContext.h"
#include "../ir/Compiled.h"

namespace fiber::script::run {

class InterpreterVm : public ExecutionContext {
public:
    InterpreterVm(const ir::Compiled &compiled,
                  const fiber::json::JsValue &root,
                  void *attach,
                  async::IScheduler *scheduler);

    async::Task<fiber::json::JsValue> exec_async();
    fiber::json::JsValue exec_sync();

    const fiber::json::JsValue &root() const override;
    void *attach() const override;
    const fiber::json::JsValue &arg_value(std::size_t index) const override;
    std::size_t arg_count() const override;
    async::IScheduler *scheduler() const override;

private:
    const ir::Compiled &compiled_;
    fiber::json::JsValue root_;
    void *attach_ = nullptr;
    async::IScheduler *scheduler_ = nullptr;
    std::vector<fiber::json::JsValue> args_;
    fiber::json::JsValue undefined_ = fiber::json::JsValue::make_undefined();
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_INTERPRETER_VM_H
