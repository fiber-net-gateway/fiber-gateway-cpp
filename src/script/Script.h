#ifndef FIBER_SCRIPT_SCRIPT_H
#define FIBER_SCRIPT_SCRIPT_H

#include <coroutine>
#include <memory>
#include <optional>

#include "../common/json/JsNode.h"
#include "ScriptResult.h"
#include "async/AsyncTask.h"
#include "async/Task.h"
#include "ir/Compiled.h"

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
    using Result = ScriptResult;

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

    ScriptRun(const ir::Compiled &compiled, const fiber::json::JsValue &root, void *attach, ScriptRuntime &runtime);

    ScriptRun(const ir::Compiled &compiled, const fiber::json::JsValue &root, void *attach, fiber::json::GcHeap &heap,
              fiber::json::GcRootSet &roots);

    AsyncTask run_async_task();

    std::unique_ptr<ScriptRuntime> owned_runtime_;
    ScriptRuntime *runtime_ = nullptr;
    std::unique_ptr<run::InterpreterVm> vm_;
};

class ScriptRun::Awaiter final {
public:
    explicit Awaiter(ScriptRun &&run);
    ~Awaiter();
    bool await_ready();
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> handle);
    Result await_resume();

private:
    static void complete(void *context, const ScriptResult &result) noexcept;

    ScriptRun run_;
    std::optional<Result> result_;
    AsyncTask task_;
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

    ScriptAsyncRun exec_async(const fiber::json::JsValue &root, void *attach, ScriptRuntime &runtime);

    ScriptAsyncRun exec_async(const fiber::json::JsValue &root, void *attach, fiber::json::GcHeap &heap,
                              fiber::json::GcRootSet &roots);

    ScriptSyncRun exec_sync(const fiber::json::JsValue &root, void *attach, ScriptRuntime &runtime);

    ScriptSyncRun exec_sync(const fiber::json::JsValue &root, void *attach, fiber::json::GcHeap &heap,
                            fiber::json::GcRootSet &roots);

    bool contains_async() const;

private:
    std::shared_ptr<ir::Compiled> compiled_;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_SCRIPT_H
