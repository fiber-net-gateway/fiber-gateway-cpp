#include "Script.h"

#include <utility>

#include "../common/Assert.h"
#include "JsGc.h"
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

ScriptRun::ScriptRun(const ir::Compiled &compiled, const fiber::script::JsValue &root, void *attach,
                     fiber::script::GcHeap &heap) :
    heap_(&heap), vm_(std::make_unique<run::InterpreterVm>(compiled, root, attach, heap)) {}

ScriptRun::Result ScriptRun::operator()() {
    if (!vm_ || !heap_) {
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
    if (!vm_ || !heap_) {
        co_return ScriptStatus::abort(ScriptAbortReason::InvalidState);
    }
    while (!vm_->done()) {
        vm_->iterate();
        if (vm_->done()) {
            break;
        }
        if (!vm_->async_task().valid()) {
            co_return ScriptStatus::abort(ScriptAbortReason::InvalidState);
        }
        co_await VmTaskAwaiter{vm_->async_task()};
    }
    if (vm_->result().is_success()) {
        co_return ScriptStatus::success();
    }
    if (vm_->result().is_exception()) {
        co_return ScriptStatus::exception();
    }
    co_return ScriptStatus::abort(vm_->result().abort().reason, vm_->result().abort().position);
}

ScriptRun::Awaiter::Awaiter(ScriptRun &&run) : run_(std::move(run)) {}

ScriptRun::Awaiter::~Awaiter() = default;

bool ScriptRun::Awaiter::await_ready() {
    if (!run_.valid()) {
        status_ = ScriptStatus::abort(ScriptAbortReason::InvalidState);
        return true;
    }
    task_ = run_.run_async_task();
    if (!task_.valid()) {
        status_ = ScriptStatus::abort(task_.allocation_failed() ? ScriptAbortReason::OutOfMemory
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
    if (!status_) {
        return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    if (run_.vm_ && run_.vm_->done()) {
        return run_.vm_->result();
    }
    if (status_->is_abort()) {
        return ScriptResult::abort(status_->abort().reason, status_->abort().position);
    }
    return ScriptResult::abort(ScriptAbortReason::InvalidState);
}

void ScriptRun::Awaiter::complete(void *context, ScriptStatus status) noexcept {
    auto *self = static_cast<Awaiter *>(context);
    if (!self) {
        return;
    }
    self->status_ = status;
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

ScriptAsyncRun Script::exec_async(const fiber::script::JsValue &root, void *attach, fiber::script::GcHeap &heap) {
    if (!compiled_) {
        return {};
    }
    return ScriptAsyncRun(ScriptRun(*compiled_, root, attach, heap));
}

ScriptSyncRun Script::exec_sync(const fiber::script::JsValue &root, void *attach, fiber::script::GcHeap &heap) {
    if (!compiled_) {
        return {};
    }
    if (compiled_->contains_async()) {
        FIBER_PANIC("async opcode encountered in exec_sync");
    }
    return ScriptSyncRun(ScriptRun(*compiled_, root, attach, heap));
}

bool Script::contains_async() const { return compiled_ && compiled_->contains_async(); }

} // namespace fiber::script
