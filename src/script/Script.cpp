#include "Script.h"

#include <string>
#include <utility>

#include "../common/json/JsGc.h"
#include "Runtime.h"
#include "run/InterpreterVm.h"
#include "run/VmError.h"

namespace fiber::script {

namespace {

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
        return fiber::json::JsValue::make_undefined();
    }
    fiber::json::JsValue value;
    value.type_ = fiber::json::JsNodeType::Exception;
    value.gc = &exc->hdr;
    return value;
}

fiber::json::JsValue make_task_error_value(fiber::json::GcHeap &heap, const async::TaskError &error) {
    run::VmError vm_error;
    vm_error.name = "EXEC_ASYNC_ERROR";
    vm_error.message = error.message.empty() ? "async task error" : error.message;
    return make_error_value(heap, vm_error);
}

} // namespace

Script::Script(std::shared_ptr<ir::Compiled> compiled)
    : compiled_(std::move(compiled)) {
}

async::Task<fiber::json::JsValue> Script::exec_async(const fiber::json::JsValue &root,
                                                     void *attach,
                                                     async::IScheduler *scheduler,
                                                     ScriptRuntime &runtime) {
    if (!compiled_) {
        co_return fiber::json::JsValue::make_undefined();
    }
    run::InterpreterVm vm(*compiled_, root, attach, scheduler, runtime);
    auto task = vm.exec_async();
    if (scheduler) {
        task.set_scheduler(scheduler);
    }
    auto result = co_await task;
    if (!result) {
        co_return make_task_error_value(runtime.heap(), result.error());
    }
    auto vm_result = std::move(result.value());
    if (!vm_result) {
        co_return make_error_value(runtime.heap(), vm_result.error());
    }
    co_return vm_result.value();
}

async::Task<fiber::json::JsValue> Script::exec_async(const fiber::json::JsValue &root,
                                                     void *attach,
                                                     async::IScheduler *scheduler,
                                                     fiber::json::GcHeap &heap,
                                                     fiber::json::GcRootSet &roots) {
    if (!compiled_) {
        co_return fiber::json::JsValue::make_undefined();
    }
    ScriptRuntime runtime(heap, roots);
    co_return co_await exec_async(root, attach, scheduler, runtime);
}

fiber::json::JsValue Script::exec_sync(const fiber::json::JsValue &root,
                                       void *attach,
                                       ScriptRuntime &runtime) {
    if (!compiled_) {
        return fiber::json::JsValue::make_undefined();
    }
    run::InterpreterVm vm(*compiled_, root, attach, nullptr, runtime);
    auto result = vm.exec_sync();
    if (!result) {
        return make_error_value(runtime.heap(), result.error());
    }
    return result.value();
}

fiber::json::JsValue Script::exec_sync(const fiber::json::JsValue &root,
                                       void *attach,
                                       fiber::json::GcHeap &heap,
                                       fiber::json::GcRootSet &roots) {
    if (!compiled_) {
        return fiber::json::JsValue::make_undefined();
    }
    ScriptRuntime runtime(heap, roots);
    return exec_sync(root, attach, runtime);
}

bool Script::contains_async() const {
    return compiled_ && compiled_->contains_async();
}

} // namespace fiber::script
