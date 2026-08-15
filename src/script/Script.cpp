#include <fiber/script/Script.h>

#include <coroutine>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/script/run/InterpreterVm.h>
#include <fiber/script/run/JitVm.h>

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

Script::Script(std::shared_ptr<ir::Compiled> compiled, std::shared_ptr<const run::JitCode> jit_code) :
    compiled_(std::move(compiled)), jit_code_(std::move(jit_code)) {}

Script::Script(std::shared_ptr<ir::Compiled> compiled, std::shared_ptr<const jit::JitCompileError> jit_error) :
    compiled_(std::move(compiled)), jit_error_(std::move(jit_error)) {}

fiber::async::Task<ScriptResult> Script::exec_async(fiber::script::JsValue root, void *attach,
                                                    fiber::script::GcHeap &heap) {
    auto compiled = compiled_;
    if (!compiled) {
        co_return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    auto jit_code = jit_code_;
    if (jit_code) {
        run::JitVm vm(std::move(jit_code), root, attach, heap);
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

ScriptResult Script::exec_sync(fiber::script::JsValue root, void *attach, fiber::script::GcHeap &heap) const {
    if (!compiled_) {
        return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    if (compiled_->contains_async()) {
        FIBER_PANIC("async opcode encountered in exec_sync");
    }
    if (jit_code_) {
        run::JitVm vm(jit_code_, root, attach, heap);
        while (!vm.done()) {
            vm.iterate();
            if (!vm.done() && vm.async_task().valid()) {
                FIBER_PANIC("async opcode encountered in exec_sync");
            }
        }
        return vm.result();
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

bool Script::contains_async() const noexcept { return compiled_ && compiled_->contains_async(); }

std::uint32_t Script::jit_inlined_operator_helper_count() const noexcept {
    return jit_code_ ? jit_code_->inlined_operator_helper_count() : 0;
}

} // namespace fiber::script
