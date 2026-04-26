#include "Library.h"

#include <memory>
#include <unordered_map>

#include "../common/Assert.h"

namespace fiber::script {

namespace {

ScriptResult script_result_from_legacy(Library::FunctionResult result) noexcept {
    if (result) {
        return ScriptResult::success(result.value());
    }
    return ScriptResult::exception(result.error());
}

class LegacyExecutionContextAdapter final : public ExecutionContext {
public:
    LegacyExecutionContextAdapter(const Library::HostCallFrame &frame, const Library::Arguments &args) :
        frame_(frame), args_(args) {}

    ScriptRuntime &runtime() override {
        FIBER_ASSERT(frame_.runtime);
        return *frame_.runtime;
    }

    const fiber::json::JsValue &root() const override {
        FIBER_ASSERT(frame_.root);
        return *frame_.root;
    }

    void *attach() const override { return frame_.attach; }

    const fiber::json::JsValue &arg_value(std::size_t index) const override {
        if (!args_.args || index >= args_.argc) {
            return undefined_;
        }
        return args_.args[index];
    }

    std::size_t arg_count() const override { return args_.argc; }

private:
    const Library::HostCallFrame &frame_;
    const Library::Arguments &args_;
    fiber::json::JsValue undefined_ = fiber::json::JsValue::make_undefined();
};

struct LegacyAsyncState final : public AsyncExecutionContext {
    LegacyAsyncState(const Library::HostCallFrame &frame, const Library::Arguments &args) : frame(frame), args(args) {}

    ScriptRuntime &runtime() override {
        FIBER_ASSERT(frame.runtime);
        return *frame.runtime;
    }

    const fiber::json::JsValue &root() const override {
        FIBER_ASSERT(frame.root);
        return *frame.root;
    }

    void *attach() const override { return frame.attach; }

    const fiber::json::JsValue &arg_value(std::size_t index) const override {
        if (!args.args || index >= args.argc) {
            return undefined;
        }
        return args.args[index];
    }

    std::size_t arg_count() const override { return args.argc; }

    void return_value(const fiber::json::JsValue &value) override { complete(ScriptResult::success(value)); }

    void throw_value(const fiber::json::JsValue &value) override { complete(ScriptResult::exception(value)); }

    void complete(ScriptResult value) noexcept {
        if (done) {
            return;
        }
        result = value;
        done = true;
        if (continuation) {
            continuation.resume();
        }
    }

    const Library::HostCallFrame &frame;
    const Library::Arguments &args;
    fiber::json::JsValue undefined = fiber::json::JsValue::make_undefined();
    ScriptResult result = ScriptResult::abort(ScriptAbortReason::InvalidState);
    std::coroutine_handle<> continuation = nullptr;
    bool done = false;
};

struct LegacyAsyncAwaiter {
    LegacyAsyncState *state = nullptr;

    bool await_ready() const noexcept { return state && state->done; }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        if (!state) {
            return false;
        }
        state->continuation = handle;
        return !state->done;
    }

    ScriptResult await_resume() const noexcept {
        if (!state) {
            return ScriptResult::abort(ScriptAbortReason::InvalidState);
        }
        return state->result;
    }
};

ScriptResult legacy_function_thunk(void *userdata, const Library::HostCallFrame &frame,
                                  const Library::Arguments &args) noexcept {
    auto *func = static_cast<Library::LegacyFunction *>(userdata);
    LegacyExecutionContextAdapter context(frame, args);
    return script_result_from_legacy(func->call(context));
}

ScriptResult legacy_constant_thunk(void *userdata, const Library::HostCallFrame &frame) noexcept {
    Library::Arguments args;
    auto *constant = static_cast<Library::LegacyConstant *>(userdata);
    LegacyExecutionContextAdapter context(frame, args);
    return script_result_from_legacy(constant->get(context));
}

AsyncTask legacy_async_function_thunk(void *userdata, const Library::HostCallFrame &frame,
                                      const Library::Arguments &args) noexcept {
    auto *func = static_cast<Library::LegacyAsyncFunction *>(userdata);
    LegacyAsyncState state(frame, args);
    func->call(state);
    co_return co_await LegacyAsyncAwaiter{&state};
}

AsyncTask legacy_async_constant_thunk(void *userdata, const Library::HostCallFrame &frame) noexcept {
    Library::Arguments args;
    auto *constant = static_cast<Library::LegacyAsyncConstant *>(userdata);
    LegacyAsyncState state(frame, args);
    constant->get(state);
    co_return co_await LegacyAsyncAwaiter{&state};
}

template<typename LegacyPtr>
const Library::HostCallable *
wrap_legacy_callable(LegacyPtr *legacy, std::unordered_map<LegacyPtr *, std::unique_ptr<Library::HostCallable>> &cache,
                     Library::HostCallable::Kind kind) {
    if (!legacy) {
        return nullptr;
    }
    auto it = cache.find(legacy);
    if (it != cache.end()) {
        return it->second.get();
    }
    auto callable = std::make_unique<Library::HostCallable>();
    callable->kind = kind;
    callable->userdata = legacy;
    switch (kind) {
        case Library::HostCallable::Kind::SyncFunction:
            callable->function = legacy_function_thunk;
            break;
        case Library::HostCallable::Kind::AsyncFunction:
            callable->async_function = legacy_async_function_thunk;
            break;
        case Library::HostCallable::Kind::SyncConstant:
            callable->constant = legacy_constant_thunk;
            break;
        case Library::HostCallable::Kind::AsyncConstant:
            callable->async_constant = legacy_async_constant_thunk;
            break;
    }
    const Library::HostCallable *ptr = callable.get();
    cache.emplace(legacy, std::move(callable));
    return ptr;
}

struct LegacyHostCallableCache {
    std::unordered_map<Library::LegacyFunction *, std::unique_ptr<Library::HostCallable>> functions;
    std::unordered_map<Library::LegacyAsyncFunction *, std::unique_ptr<Library::HostCallable>> async_functions;
    std::unordered_map<Library::LegacyConstant *, std::unique_ptr<Library::HostCallable>> constants;
    std::unordered_map<Library::LegacyAsyncConstant *, std::unique_ptr<Library::HostCallable>> async_constants;
};

LegacyHostCallableCache &host_cache(const Library *library) {
    static std::unordered_map<const Library *, LegacyHostCallableCache> caches;
    return caches[library];
}

} // namespace

Library::FunctionMatchResult Library::FunctionMatchResult::not_found() noexcept {
    FunctionMatchResult result;
    result.status = FunctionMatchStatus::NotFound;
    return result;
}

Library::FunctionMatchResult Library::FunctionMatchResult::arity_mismatch() noexcept {
    FunctionMatchResult result;
    result.status = FunctionMatchStatus::ArityMismatch;
    return result;
}

Library::FunctionMatchResult Library::FunctionMatchResult::ambiguous() noexcept {
    FunctionMatchResult result;
    result.status = FunctionMatchStatus::Ambiguous;
    return result;
}

Library::FunctionMatchResult Library::FunctionMatchResult::found(const HostCallable *callable,
                                                                 FunctionSignature signature,
                                                                 const fiber::json::JsValue *defaults,
                                                                 std::uint16_t default_count) noexcept {
    FunctionMatchResult result;
    result.status = FunctionMatchStatus::Found;
    result.callable = callable;
    result.signature = signature;
    result.defaults_to_append = defaults;
    result.default_count = default_count;
    return result;
}

Library::FunctionResult Library::LegacyConstant::get(ExecutionContext &context) {
    (void) context;
    return std::unexpected(fiber::json::JsValue::make_undefined());
}

Library::FunctionResult Library::LegacyFunction::call(ExecutionContext &context) {
    (void) context;
    return std::unexpected(fiber::json::JsValue::make_undefined());
}

void Library::LegacyAsyncConstant::get(AsyncExecutionContext &context) { (void) context; }

void Library::LegacyAsyncFunction::call(AsyncExecutionContext &context) { (void) context; }

Library::FunctionMatchResult Library::resolve_func(std::string_view name,
                                                   const FunctionMatchRequest &request) const {
    return const_cast<Library *>(this)->find_func(name, request);
}

Library::FunctionMatchResult Library::resolve_async_func(std::string_view name,
                                                         const FunctionMatchRequest &request) const {
    return const_cast<Library *>(this)->find_async_func(name, request);
}

const Library::HostCallable *Library::resolve_constant(std::string_view namespace_name,
                                                       std::string_view key) const {
    return host_callable_for(const_cast<Library *>(this)->find_constant(namespace_name, key));
}

const Library::HostCallable *Library::resolve_async_constant(std::string_view namespace_name,
                                                             std::string_view key) const {
    return host_callable_for(const_cast<Library *>(this)->find_async_constant(namespace_name, key));
}

const Library::HostCallable *Library::host_callable_for(LegacyFunction *func) const {
    return wrap_legacy_callable(func, host_cache(this).functions, HostCallable::Kind::SyncFunction);
}

const Library::HostCallable *Library::host_callable_for(LegacyAsyncFunction *func) const {
    return wrap_legacy_callable(func, host_cache(this).async_functions, HostCallable::Kind::AsyncFunction);
}

const Library::HostCallable *Library::host_callable_for(LegacyConstant *constant) const {
    return wrap_legacy_callable(constant, host_cache(this).constants, HostCallable::Kind::SyncConstant);
}

const Library::HostCallable *Library::host_callable_for(LegacyAsyncConstant *constant) const {
    return wrap_legacy_callable(constant, host_cache(this).async_constants, HostCallable::Kind::AsyncConstant);
}

} // namespace fiber::script
