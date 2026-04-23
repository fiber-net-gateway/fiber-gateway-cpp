#include "Script.h"

#include <string_view>
#include <utility>

#include "../common/Assert.h"
#include "Runtime.h"
#include "run/InterpreterVm.h"
#include "run/VmError.h"

namespace fiber::script {

namespace {

ScriptAbortReason abort_reason_from_vm_error(const run::VmError &error) noexcept {
    std::string_view name = error.name;
    if (name == "EXEC_OUT_OF_MEMORY") {
        return ScriptAbortReason::OutOfMemory;
    }
    if (name == "EXEC_TYPE_ERROR") {
        return ScriptAbortReason::TypeError;
    }
    if (name == "EXEC_INDEX_ERROR") {
        return ScriptAbortReason::IndexError;
    }
    if (name == "EXEC_DIVISION_BY_ZERO") {
        return ScriptAbortReason::DivisionByZero;
    }
    if (name == "EXEC_UNKNOWN_IDENTIFIER") {
        return ScriptAbortReason::UnknownIdentifier;
    }
    if (name == "EXEC_UNKNOWN_OPCODE") {
        return ScriptAbortReason::InvalidOpcode;
    }
    if (name == "EXEC_NO_RETURN") {
        return ScriptAbortReason::NoReturn;
    }
    if (name == "HOST_INVALID_STATE") {
        return ScriptAbortReason::InvalidState;
    }
    if (!name.empty() && name.starts_with("HOST_")) {
        return ScriptAbortReason::HostFault;
    }
    return ScriptAbortReason::Internal;
}

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
    run::VmResult vm_out = fiber::json::JsValue::make_undefined();
    auto state = vm_->iterate(vm_out);
    if (state == run::InterpreterVm::VmState::Suspend) {
        FIBER_PANIC("async opcode encountered in exec_sync");
    }
    return to_result(std::move(vm_out));
}

ScriptRun::Awaiter::Awaiter(ScriptRun &&run) : run_(std::move(run)) {
    if (run_.vm_) {
        run_.vm_->set_resume_callback(&ScriptRun::Awaiter::resume_callback, this);
    }
}

ScriptRun::Awaiter::~Awaiter() {
    if (run_.vm_) {
        run_.vm_->set_resume_callback(nullptr, nullptr);
    }
}

bool ScriptRun::Awaiter::await_ready() {
    if (!run_.valid()) {
        result_ = ScriptResult::abort(ScriptAbortReason::InvalidState);
        return true;
    }
    return false;
}

bool ScriptRun::Awaiter::await_suspend(std::coroutine_handle<> handle) {
    continuation_ = handle;
    if (pump()) {
        return false;
    }
    return true;
}

ScriptRun::Result ScriptRun::Awaiter::await_resume() {
    if (!result_) {
        return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    return std::move(*result_);
}

void ScriptRun::Awaiter::resume_callback(void *context) {
    auto *self = static_cast<Awaiter *>(context);
    if (!self) {
        return;
    }
    self->resume_if_complete();
}

bool ScriptRun::Awaiter::pump() {
    if (!run_.vm_ || !run_.runtime_) {
        result_ = ScriptResult::abort(ScriptAbortReason::InvalidState);
        return true;
    }
    run::VmResult vm_out = fiber::json::JsValue::make_undefined();
    auto state = run_.vm_->iterate(vm_out);
    if (state == run::InterpreterVm::VmState::Success || state == run::InterpreterVm::VmState::Error) {
        result_ = run_.to_result(std::move(vm_out));
        run_.vm_->set_resume_callback(nullptr, nullptr);
        return true;
    }
    return false;
}

void ScriptRun::Awaiter::resume_if_complete() {
    if (resuming_) {
        return;
    }
    resuming_ = true;
    if (pump()) {
        auto handle = continuation_;
        resuming_ = false;
        if (!handle) {
            return;
        }
        handle.resume();
        return;
    }
    resuming_ = false;
}

ScriptRun::Awaiter ScriptRun::operator co_await() && { return Awaiter(std::move(*this)); }

bool ScriptRun::valid() const { return static_cast<bool>(vm_); }

ScriptRun::Result ScriptRun::to_result(run::VmResult result) {
    if (result) {
        return ScriptResult::success(result.value());
    }
    if (result.error().kind == run::VmErrorKind::Thrown && vm_ && vm_->has_thrown()) {
        return ScriptResult::exception(vm_->thrown());
    }
    return ScriptResult::abort(abort_reason_from_vm_error(result.error()), result.error().position);
}

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
