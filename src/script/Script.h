#ifndef FIBER_SCRIPT_SCRIPT_H
#define FIBER_SCRIPT_SCRIPT_H

#include <coroutine>
#include <expected>
#include <memory>
#include <optional>

#include "../common/json/JsNode.h"
#include "async/AsyncExecutionContext.h"
#include "async/Task.h"
#include "ir/Compiled.h"
#include "run/VmError.h"

namespace fiber::json {
class GcHeap;
class GcRootSet;
} // namespace fiber::json

namespace fiber::script {

class ScriptRuntime;
namespace run {
class InterpreterVm;
} // namespace run

class ScriptRun {
public:
    using Result = std::expected<fiber::json::JsValue, fiber::json::JsValue>;

    class Awaiter;

    ScriptRun();
    ScriptRun(const ScriptRun &) = delete;
    ScriptRun &operator=(const ScriptRun &) = delete;
    ScriptRun(ScriptRun &&) noexcept;
    ScriptRun &operator=(ScriptRun &&) noexcept;
    ~ScriptRun();

    Result operator()();

    Awaiter operator co_await() &&;

    bool valid() const;

private:
    friend class Script;

    ScriptRun(const ir::Compiled &compiled,
              const fiber::json::JsValue &root,
              void *attach,
              async::IScheduler *scheduler,
              ScriptRuntime &runtime);

    ScriptRun(const ir::Compiled &compiled,
              const fiber::json::JsValue &root,
              void *attach,
              async::IScheduler *scheduler,
              fiber::json::GcHeap &heap,
              fiber::json::GcRootSet &roots);

    Result to_result(run::VmResult result);

    std::unique_ptr<ScriptRuntime> owned_runtime_;
    ScriptRuntime *runtime_ = nullptr;
    std::unique_ptr<run::InterpreterVm> vm_;
    async::IScheduler *scheduler_ = nullptr;
};

class ScriptRun::Awaiter final : public AsyncExecutionContext {
public:
    explicit Awaiter(ScriptRun &&run);
    bool await_ready();
    bool await_suspend(std::coroutine_handle<> handle);
    Result await_resume();

private:
    ScriptRuntime &runtime() override;
    const fiber::json::JsValue &root() const override;
    void *attach() const override;
    const fiber::json::JsValue &arg_value(std::size_t index) const override;
    std::size_t arg_count() const override;
    async::IScheduler *scheduler() const override;
    void return_value(const fiber::json::JsValue &value) override;
    void throw_value(const fiber::json::JsValue &value) override;

    bool pump();
    void resume_if_complete();

    ScriptRun run_;
    std::optional<Result> result_;
    std::coroutine_handle<> continuation_ = nullptr;
    bool in_exec_ = false;
    bool resuming_ = false;
};

class Script {
public:
    Script() = default;
    explicit Script(std::shared_ptr<ir::Compiled> compiled);

    ScriptRun exec_async(const fiber::json::JsValue &root,
                         void *attach,
                         async::IScheduler *scheduler,
                         ScriptRuntime &runtime);

    ScriptRun exec_async(const fiber::json::JsValue &root,
                         void *attach,
                         ScriptRuntime &runtime) {
        return exec_async(root, attach, nullptr, runtime);
    }

    ScriptRun exec_async(const fiber::json::JsValue &root,
                         void *attach,
                         async::IScheduler *scheduler,
                         fiber::json::GcHeap &heap,
                         fiber::json::GcRootSet &roots);

    ScriptRun exec_async(const fiber::json::JsValue &root,
                         void *attach,
                         fiber::json::GcHeap &heap,
                         fiber::json::GcRootSet &roots) {
        return exec_async(root, attach, nullptr, heap, roots);
    }

    ScriptRun exec_sync(const fiber::json::JsValue &root,
                        void *attach,
                        ScriptRuntime &runtime);

    ScriptRun exec_sync(const fiber::json::JsValue &root,
                        void *attach,
                        fiber::json::GcHeap &heap,
                        fiber::json::GcRootSet &roots);

    bool contains_async() const;

private:
    std::shared_ptr<ir::Compiled> compiled_;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_SCRIPT_H
