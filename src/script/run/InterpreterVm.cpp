#include "InterpreterVm.h"

namespace fiber::script::run {

InterpreterVm::InterpreterVm(const ir::Compiled &compiled,
                             const fiber::json::JsValue &root,
                             void *attach,
                             async::IScheduler *scheduler)
    : compiled_(compiled), root_(root), attach_(attach), scheduler_(scheduler) {
}

async::Task<fiber::json::JsValue> InterpreterVm::exec_async() {
    co_return fiber::json::JsValue::make_undefined();
}

fiber::json::JsValue InterpreterVm::exec_sync() {
    return fiber::json::JsValue::make_undefined();
}

const fiber::json::JsValue &InterpreterVm::root() const {
    return root_;
}

void *InterpreterVm::attach() const {
    return attach_;
}

const fiber::json::JsValue &InterpreterVm::arg_value(std::size_t index) const {
    if (index >= args_.size()) {
        return undefined_;
    }
    return args_[index];
}

std::size_t InterpreterVm::arg_count() const {
    return args_.size();
}

async::IScheduler *InterpreterVm::scheduler() const {
    return scheduler_;
}

} // namespace fiber::script::run
