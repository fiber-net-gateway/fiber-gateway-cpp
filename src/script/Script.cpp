#include "Script.h"

#include <coroutine>
#include <utility>

#include "../common/Assert.h"
#include "run/InterpreterVm.h"

namespace fiber::script {

namespace {

struct SwitchAsyncAwaiter {
    AsyncTask &task;

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
        return task.swap_coroutine_handle(continuation);
    }

    void await_resume() const noexcept {}
};

} // namespace

Script::Script(std::shared_ptr<ir::Compiled> compiled) : compiled_(std::move(compiled)) {}

fiber::async::Task<ScriptResult> Script::exec_async(fiber::script::JsValue root, void *attach,
                                                 fiber::script::GcHeap &heap) {
    auto compiled = compiled_;
    if (!compiled) {
        co_return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    run::InterpreterVm vm(*compiled, root, attach, heap);
    while (!vm.done()) {
        vm.iterate();
        if (vm.done()) {
            break;
        }
        if (!vm.async_task().valid()) {
            co_return ScriptResult::abort(ScriptAbortReason::InvalidState);
        }
        co_await SwitchAsyncAwaiter{vm.async_task()};
    }
    co_return vm.result();
}

ScriptResult Script::exec_sync(fiber::script::JsValue root, void *attach, fiber::script::GcHeap &heap) {
    if (!compiled_) {
        return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    if (compiled_->contains_async()) {
        FIBER_PANIC("async opcode encountered in exec_sync");
    }
    run::InterpreterVm vm(*compiled_, root, attach, heap);
    while (!vm.done()) {
        vm.iterate();
        if (!vm.done() && vm.async_task().valid()) {
            FIBER_PANIC("async opcode encountered in exec_sync");
        }
    }
    return vm.result();
}

bool Script::contains_async() const { return compiled_ && compiled_->contains_async(); }

} // namespace fiber::script
