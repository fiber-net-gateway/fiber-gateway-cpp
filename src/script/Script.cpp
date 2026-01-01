#include "Script.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#include "../common/json/JsGc.h"
#include "Runtime.h"
#include "run/InterpreterVm.h"
#include "run/VmError.h"

namespace fiber::script {

namespace {

constexpr const char kExecError[] = "EXEC_ERROR";
constexpr const char kOutOfMemory[] = "EXEC_OUT_OF_MEMORY";

[[noreturn]] void panic_async_in_sync() {
    std::abort();
}

fiber::json::JsValue make_fallback_error(std::string_view message) {
    return fiber::json::JsValue::make_native_string(const_cast<char *>(message.data()), message.size());
}

fiber::json::JsValue make_error_value(fiber::json::GcHeap &heap, const run::VmError &error) {
    std::string name = error.name.empty() ? "EXEC_ERROR" : error.name;
    std::string message = error.message.empty() ? "script error" : error.message;
    fiber::json::GcException *exc = fiber::json::gc_new_exception(&heap,
                                                                  error.position,
                                                                  name.c_str(),
                                                                  name.size(),
                                                                  message.c_str(),
                                                                  message.size(),
                                                                  error.meta);
    if (!exc) {
        return make_fallback_error(kOutOfMemory);
    }
    fiber::json::JsValue value;
    value.type_ = fiber::json::JsNodeType::Exception;
    value.gc = &exc->hdr;
    return value;
}

} // namespace

ScriptRun::ScriptRun() = default;

ScriptRun::ScriptRun(ScriptRun &&) noexcept = default;

ScriptRun &ScriptRun::operator=(ScriptRun &&) noexcept = default;

ScriptRun::~ScriptRun() = default;

ScriptRun::ScriptRun(const ir::Compiled &compiled,
                     const fiber::json::JsValue &root,
                     void *attach,
                     async::IScheduler *scheduler,
                     ScriptRuntime &runtime)
    : runtime_(&runtime),
      vm_(std::make_unique<run::InterpreterVm>(compiled, root, attach, scheduler, runtime)),
      scheduler_(scheduler) {
}

ScriptRun::ScriptRun(const ir::Compiled &compiled,
                     const fiber::json::JsValue &root,
                     void *attach,
                     async::IScheduler *scheduler,
                     fiber::json::GcHeap &heap,
                     fiber::json::GcRootSet &roots)
    : owned_runtime_(std::make_unique<ScriptRuntime>(heap, roots)),
      runtime_(owned_runtime_.get()),
      vm_(std::make_unique<run::InterpreterVm>(compiled, root, attach, scheduler, *runtime_)),
      scheduler_(scheduler) {
}

ScriptRun::Result ScriptRun::operator()() {
    if (!vm_ || !runtime_) {
        return fiber::json::JsValue::make_undefined();
    }
    auto result = vm_->exec_sync();
    return to_result(std::move(result));
}

ScriptRun::Awaiter::Awaiter(ScriptRun &&run)
    : run_(std::move(run)) {
}

bool ScriptRun::Awaiter::await_ready() {
    if (!run_.valid()) {
        result_ = fiber::json::JsValue::make_undefined();
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
        return fiber::json::JsValue::make_undefined();
    }
    return std::move(*result_);
}

ScriptRuntime &ScriptRun::Awaiter::runtime() {
    return *run_.runtime_;
}

const fiber::json::JsValue &ScriptRun::Awaiter::root() const {
    return run_.vm_->root();
}

void *ScriptRun::Awaiter::attach() const {
    return run_.vm_->attach();
}

const fiber::json::JsValue &ScriptRun::Awaiter::arg_value(std::size_t index) const {
    return run_.vm_->arg_value(index);
}

std::size_t ScriptRun::Awaiter::arg_count() const {
    return run_.vm_->arg_count();
}

async::IScheduler *ScriptRun::Awaiter::scheduler() const {
    return run_.scheduler_;
}

void ScriptRun::Awaiter::return_value(const fiber::json::JsValue &value) {
    if (!run_.vm_) {
        return;
    }
    run_.vm_->set_async_result(value, false);
    if (in_exec_) {
        return;
    }
    resume_if_complete();
}

void ScriptRun::Awaiter::throw_value(const fiber::json::JsValue &value) {
    if (!run_.vm_) {
        return;
    }
    run_.vm_->set_async_result(value, true);
    if (in_exec_) {
        return;
    }
    resume_if_complete();
}

bool ScriptRun::Awaiter::pump() {
    if (!run_.vm_ || !run_.runtime_) {
        result_ = fiber::json::JsValue::make_undefined();
        return true;
    }
    in_exec_ = true;
    run::VmResult vm_out = fiber::json::JsValue::make_undefined();
    auto state = run_.vm_->exec_async(*this, vm_out);
    in_exec_ = false;
    if (state == run::InterpreterVm::AsyncExecState::Done) {
        result_ = run_.to_result(std::move(vm_out));
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
        if (run_.scheduler_) {
            run_.scheduler_->post(handle);
        } else {
            handle.resume();
        }
        return;
    }
    resuming_ = false;
}

ScriptRun::Awaiter ScriptRun::operator co_await() && {
    return Awaiter(std::move(*this));
}

bool ScriptRun::valid() const {
    return static_cast<bool>(vm_);
}

ScriptRun::Result ScriptRun::to_result(run::VmResult result) {
    if (result) {
        return result.value();
    }
    if (result.error().kind == run::VmErrorKind::Thrown && vm_ && vm_->has_thrown()) {
        return std::unexpected(vm_->thrown());
    }
    if (!runtime_) {
        return std::unexpected(make_fallback_error(kExecError));
    }
    return std::unexpected(make_error_value(runtime_->heap(), result.error()));
}

Script::Script(std::shared_ptr<ir::Compiled> compiled)
    : compiled_(std::move(compiled)) {
}

ScriptRun Script::exec_async(const fiber::json::JsValue &root,
                             void *attach,
                             async::IScheduler *scheduler,
                             ScriptRuntime &runtime) {
    if (!compiled_) {
        return {};
    }
    return ScriptRun(*compiled_, root, attach, scheduler, runtime);
}

ScriptRun Script::exec_async(const fiber::json::JsValue &root,
                             void *attach,
                             async::IScheduler *scheduler,
                             fiber::json::GcHeap &heap,
                             fiber::json::GcRootSet &roots) {
    if (!compiled_) {
        return {};
    }
    return ScriptRun(*compiled_, root, attach, scheduler, heap, roots);
}

ScriptRun Script::exec_sync(const fiber::json::JsValue &root,
                            void *attach,
                            ScriptRuntime &runtime) {
    if (!compiled_) {
        return {};
    }
    if (compiled_->contains_async()) {
        panic_async_in_sync();
    }
    return ScriptRun(*compiled_, root, attach, nullptr, runtime);
}

ScriptRun Script::exec_sync(const fiber::json::JsValue &root,
                            void *attach,
                            fiber::json::GcHeap &heap,
                            fiber::json::GcRootSet &roots) {
    if (!compiled_) {
        return {};
    }
    if (compiled_->contains_async()) {
        panic_async_in_sync();
    }
    return ScriptRun(*compiled_, root, attach, nullptr, heap, roots);
}

bool Script::contains_async() const {
    return compiled_ && compiled_->contains_async();
}

} // namespace fiber::script
