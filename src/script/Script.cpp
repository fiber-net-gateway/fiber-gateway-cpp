#include "Script.h"

#include <utility>

#include "../common/Assert.h"
#include "Runtime.h"
#include "run/InterpreterVm.h"

namespace fiber::script {

namespace {

struct VmTaskAwaiter {
    AsyncTask &task;

    bool await_ready() const noexcept { return !task.valid(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
        return task.swap_coroutine_handle(continuation);
    }

    void await_resume() const noexcept {}
};

} // namespace

ScriptRun::ScriptRun() = default;

ScriptRun::ScriptRun(ScriptRun &&) noexcept = default;

ScriptRun &ScriptRun::operator=(ScriptRun &&) noexcept = default;

ScriptRun::~ScriptRun() = default;

ScriptRun::ScriptRun(const ir::Compiled &compiled, const fiber::json::JsValue &root, void *attach,
                     ScriptRuntime &runtime) :
    runtime_(&runtime), vm_(std::make_unique<run::InterpreterVm>(compiled, root, attach, runtime)) {}

ScriptRun::ScriptRun(const ir::Compiled &compiled, const fiber::json::JsValue &root, void *attach,
                     fiber::json::GcHeap &heap, fiber::json::GcRootSet &roots) :
    owned_runtime_(std::make_unique<ScriptRuntime>(heap, roots)), runtime_(owned_runtime_.get()),
    vm_(std::make_unique<run::InterpreterVm>(compiled, root, attach, *runtime_)) {}

ScriptRun::Result ScriptRun::operator()() {
    if (!vm_ || !runtime_) {
        return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    while (!vm_->done()) {
        vm_->iterate();
        if (!vm_->done() && vm_->async_task().valid()) {
            FIBER_PANIC("async opcode encountered in exec_sync");
        }
    }
    return vm_->result();
}

AsyncTask ScriptRun::run_async_task() {
    if (!vm_ || !runtime_) {
        co_return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    while (!vm_->done()) {
        vm_->iterate();
        if (vm_->done()) {
            break;
        }
        if (!vm_->async_task().valid()) {
            co_return ScriptResult::abort(ScriptAbortReason::InvalidState);
        }
        co_await VmTaskAwaiter{vm_->async_task()};
    }
    co_return vm_->result();
}

ScriptRun::Awaiter::Awaiter(ScriptRun &&run) : run_(std::move(run)) {}

ScriptRun::Awaiter::~Awaiter() = default;

bool ScriptRun::Awaiter::await_ready() {
    if (!run_.valid()) {
        result_ = ScriptResult::abort(ScriptAbortReason::InvalidState);
        return true;
    }
    task_ = run_.run_async_task();
    if (!task_.valid()) {
        result_ = ScriptResult::abort(task_.allocation_failed() ? ScriptAbortReason::OutOfMemory
                                                                : ScriptAbortReason::InvalidState);
        return true;
    }
    return false;
}

std::coroutine_handle<> ScriptRun::Awaiter::await_suspend(std::coroutine_handle<> handle) {
    task_.set_completion({&ScriptRun::Awaiter::complete, this});
    return task_.swap_coroutine_handle(handle);
}

ScriptRun::Result ScriptRun::Awaiter::await_resume() {
    if (!result_) {
        return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    return std::move(*result_);
}

void ScriptRun::Awaiter::complete(void *context, const ScriptResult &result) noexcept {
    auto *self = static_cast<Awaiter *>(context);
    if (!self) {
        return;
    }
    self->result_ = result;
}

ScriptRun::Awaiter ScriptRun::operator co_await() && { return Awaiter(std::move(*this)); }

bool ScriptRun::valid() const { return static_cast<bool>(vm_); }

ScriptSyncRun::ScriptSyncRun(ScriptRun run) : run_(std::move(run)) {}

ScriptSyncRun::Result ScriptSyncRun::operator()() { return run_(); }

ScriptRun::Awaiter ScriptSyncRun::operator co_await() && { return std::move(run_).operator co_await(); }

bool ScriptSyncRun::valid() const { return run_.valid(); }

ScriptAsyncRun::ScriptAsyncRun(ScriptRun run) : run_(std::move(run)) {}

ScriptRun::Awaiter ScriptAsyncRun::operator co_await() && { return std::move(run_).operator co_await(); }

bool ScriptAsyncRun::valid() const { return run_.valid(); }

Script::Script(std::shared_ptr<ir::Compiled> compiled) : compiled_(std::move(compiled)) {}

ScriptAsyncRun Script::exec_async(const fiber::json::JsValue &root, void *attach, ScriptRuntime &runtime) {
    if (!compiled_) {
        return {};
    }
    return ScriptAsyncRun(ScriptRun(*compiled_, root, attach, runtime));
}

ScriptAsyncRun Script::exec_async(const fiber::json::JsValue &root, void *attach, fiber::json::GcHeap &heap,
                                  fiber::json::GcRootSet &roots) {
    if (!compiled_) {
        return {};
    }
    return ScriptAsyncRun(ScriptRun(*compiled_, root, attach, heap, roots));
}

ScriptSyncRun Script::exec_sync(const fiber::json::JsValue &root, void *attach, ScriptRuntime &runtime) {
    if (!compiled_) {
        return {};
    }
    if (compiled_->contains_async()) {
        FIBER_PANIC("async opcode encountered in exec_sync");
    }
    return ScriptSyncRun(ScriptRun(*compiled_, root, attach, runtime));
}

ScriptSyncRun Script::exec_sync(const fiber::json::JsValue &root, void *attach, fiber::json::GcHeap &heap,
                                fiber::json::GcRootSet &roots) {
    if (!compiled_) {
        return {};
    }
    if (compiled_->contains_async()) {
        FIBER_PANIC("async opcode encountered in exec_sync");
    }
    return ScriptSyncRun(ScriptRun(*compiled_, root, attach, heap, roots));
}

bool Script::contains_async() const { return compiled_ && compiled_->contains_async(); }

} // namespace fiber::script
