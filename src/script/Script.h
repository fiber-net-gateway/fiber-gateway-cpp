#ifndef FIBER_SCRIPT_SCRIPT_H
#define FIBER_SCRIPT_SCRIPT_H

#include <coroutine>
#include <expected>
#include <memory>
#include <optional>

#include "../common/json/JsNode.h"
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
              ScriptRuntime &runtime);

    ScriptRun(const ir::Compiled &compiled,
              const fiber::json::JsValue &root,
              void *attach,
              fiber::json::GcHeap &heap,
              fiber::json::GcRootSet &roots);

    Result to_result(run::VmResult result);

    std::unique_ptr<ScriptRuntime> owned_runtime_;
    ScriptRuntime *runtime_ = nullptr;
    std::unique_ptr<run::InterpreterVm> vm_;
};

class ScriptRun::Awaiter final {
public:
    explicit Awaiter(ScriptRun &&run);
    ~Awaiter();
    bool await_ready();
    bool await_suspend(std::coroutine_handle<> handle);
    Result await_resume();

private:
    static void resume_callback(void *context);
    bool pump();
    void resume_if_complete();

    ScriptRun run_;
    std::optional<Result> result_;
    std::coroutine_handle<> continuation_ = nullptr;
    bool resuming_ = false;
};

class ScriptSyncRun {
public:
    using Result = ScriptRun::Result;

    ScriptSyncRun() = default;
    ScriptSyncRun(const ScriptSyncRun &) = delete;
    ScriptSyncRun &operator=(const ScriptSyncRun &) = delete;
    ScriptSyncRun(ScriptSyncRun &&) noexcept = default;
    ScriptSyncRun &operator=(ScriptSyncRun &&) noexcept = default;
    ~ScriptSyncRun() = default;

    Result operator()();
    ScriptRun::Awaiter operator co_await() &&;
    bool valid() const;

private:
    friend class Script;
    explicit ScriptSyncRun(ScriptRun run);

    ScriptRun run_;
};

class ScriptAsyncRun {
public:
    using Result = ScriptRun::Result;

    ScriptAsyncRun() = default;
    ScriptAsyncRun(const ScriptAsyncRun &) = delete;
    ScriptAsyncRun &operator=(const ScriptAsyncRun &) = delete;
    ScriptAsyncRun(ScriptAsyncRun &&) noexcept = default;
    ScriptAsyncRun &operator=(ScriptAsyncRun &&) noexcept = default;
    ~ScriptAsyncRun() = default;

    ScriptRun::Awaiter operator co_await() &&;
    bool valid() const;

private:
    friend class Script;
    explicit ScriptAsyncRun(ScriptRun run);

    ScriptRun run_;
};

class Script {
public:
    Script() = default;
    explicit Script(std::shared_ptr<ir::Compiled> compiled);

    ScriptAsyncRun exec_async(const fiber::json::JsValue &root,
                              void *attach,
                              ScriptRuntime &runtime);

    ScriptAsyncRun exec_async(const fiber::json::JsValue &root,
                              void *attach,
                              fiber::json::GcHeap &heap,
                              fiber::json::GcRootSet &roots);

    ScriptSyncRun exec_sync(const fiber::json::JsValue &root,
                            void *attach,
                            ScriptRuntime &runtime);

    ScriptSyncRun exec_sync(const fiber::json::JsValue &root,
                            void *attach,
                            fiber::json::GcHeap &heap,
                            fiber::json::GcRootSet &roots);

    bool contains_async() const;

private:
    std::shared_ptr<ir::Compiled> compiled_;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_SCRIPT_H
