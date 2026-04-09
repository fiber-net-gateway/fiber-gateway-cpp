#include "Library.h"

#include <exception>
#include <memory>
#include <unordered_map>

namespace fiber::script {

namespace {

class LegacyExecutionContextAdapter final : public ExecutionContext {
public:
    explicit LegacyExecutionContextAdapter(const Library::HostCallFrame &frame)
        : frame_(frame) {
    }

    ScriptRuntime &runtime() override {
        return *frame_.runtime;
    }

    const fiber::json::JsValue &root() const override {
        return *frame_.root;
    }

    void *attach() const override {
        return frame_.attach;
    }

    const fiber::json::JsValue &arg_value(std::size_t index) const override {
        if (!frame_.args || index >= frame_.argc) {
            return undefined_;
        }
        return frame_.args[index];
    }

    std::size_t arg_count() const override {
        return frame_.argc;
    }

private:
    const Library::HostCallFrame &frame_;
    fiber::json::JsValue undefined_ = fiber::json::JsValue::make_undefined();
};

class LegacyAsyncExecutionContextAdapter final : public AsyncExecutionContext {
public:
    LegacyAsyncExecutionContextAdapter(const Library::HostCallFrame &frame,
                                       const Library::HostAsyncCompletion &completion)
        : frame_(frame), completion_(completion) {
    }

    ScriptRuntime &runtime() override {
        return *frame_.runtime;
    }

    const fiber::json::JsValue &root() const override {
        return *frame_.root;
    }

    void *attach() const override {
        return frame_.attach;
    }

    const fiber::json::JsValue &arg_value(std::size_t index) const override {
        if (!frame_.args || index >= frame_.argc) {
            return undefined_;
        }
        return frame_.args[index];
    }

    std::size_t arg_count() const override {
        return frame_.argc;
    }

    void return_value(const fiber::json::JsValue &value) override {
        if (done_) {
            return;
        }
        done_ = true;
        result_ = Library::HostCallResult::returned(value);
        if (detached_ && completion_.complete) {
            completion_.complete(completion_.ctx, result_);
        }
        if (detached_) {
            delete this;
        }
    }

    void throw_value(const fiber::json::JsValue &value) override {
        if (done_) {
            return;
        }
        done_ = true;
        result_ = Library::HostCallResult::thrown(value);
        if (detached_ && completion_.complete) {
            completion_.complete(completion_.ctx, result_);
        }
        if (detached_) {
            delete this;
        }
    }

    bool done() const {
        return done_;
    }

    const Library::HostCallResult &result() const {
        return result_;
    }

    void detach() {
        detached_ = true;
        if (done_) {
            if (completion_.complete) {
                completion_.complete(completion_.ctx, result_);
            }
            delete this;
        }
    }

private:
    const Library::HostCallFrame &frame_;
    const Library::HostAsyncCompletion &completion_;
    bool done_ = false;
    bool detached_ = false;
    Library::HostCallResult result_ = Library::HostCallResult::pending();
    fiber::json::JsValue undefined_ = fiber::json::JsValue::make_undefined();
};

template <typename T>
Library::HostCallResult make_internal_fault(T *object, std::string_view debug_name) {
    (void)object;
    Library::HostFault fault;
    fault.code = Library::HostFaultCode::Internal;
    fault.name = "HOST_INTERNAL";
    fault.message = debug_name.empty() ? "host callable threw a C++ exception" : debug_name;
    fault.status = 500;
    return Library::HostCallResult::faulted(fault);
}

Library::HostCallResult legacy_function_thunk(void *userdata,
                                              const Library::HostCallFrame &frame) noexcept {
    auto *func = static_cast<Library::Function *>(userdata);
    try {
        LegacyExecutionContextAdapter context(frame);
        auto result = func->call(context);
        if (result) {
            return Library::HostCallResult::returned(result.value());
        }
        return Library::HostCallResult::thrown(result.error());
    } catch (...) {
        return make_internal_fault(func, "legacy function threw");
    }
}

Library::HostCallResult legacy_constant_thunk(void *userdata,
                                              const Library::HostCallFrame &frame) noexcept {
    auto *constant = static_cast<Library::Constant *>(userdata);
    try {
        LegacyExecutionContextAdapter context(frame);
        auto result = constant->get(context);
        if (result) {
            return Library::HostCallResult::returned(result.value());
        }
        return Library::HostCallResult::thrown(result.error());
    } catch (...) {
        return make_internal_fault(constant, "legacy constant threw");
    }
}

Library::HostCallResult legacy_async_function_thunk(void *userdata,
                                                    const Library::HostCallFrame &frame,
                                                    const Library::HostAsyncCompletion &completion) noexcept {
    auto *func = static_cast<Library::AsyncFunction *>(userdata);
    auto *context = new LegacyAsyncExecutionContextAdapter(frame, completion);
    try {
        func->call(*context);
        if (context->done()) {
            Library::HostCallResult result = context->result();
            delete context;
            return result;
        }
        context->detach();
        return Library::HostCallResult::pending();
    } catch (...) {
        delete context;
        return make_internal_fault(func, "legacy async function threw");
    }
}

Library::HostCallResult legacy_async_constant_thunk(void *userdata,
                                                    const Library::HostCallFrame &frame,
                                                    const Library::HostAsyncCompletion &completion) noexcept {
    auto *constant = static_cast<Library::AsyncConstant *>(userdata);
    auto *context = new LegacyAsyncExecutionContextAdapter(frame, completion);
    try {
        constant->get(*context);
        if (context->done()) {
            Library::HostCallResult result = context->result();
            delete context;
            return result;
        }
        context->detach();
        return Library::HostCallResult::pending();
    } catch (...) {
        delete context;
        return make_internal_fault(constant, "legacy async constant threw");
    }
}

template <typename LegacyPtr>
const Library::HostCallable *wrap_legacy_callable(
    LegacyPtr *legacy,
    std::unordered_map<LegacyPtr *, std::unique_ptr<Library::HostCallable>> &cache,
    typename Library::HostCallable::Kind kind,
    Library::HostSyncThunk sync_thunk,
    Library::HostAsyncThunk async_thunk) {
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
    callable->sync = sync_thunk;
    callable->async = async_thunk;
    const Library::HostCallable *ptr = callable.get();
    cache.emplace(legacy, std::move(callable));
    return ptr;
}

struct LegacyHostCallableCache {
    std::unordered_map<Library::Function *, std::unique_ptr<Library::HostCallable>> functions;
    std::unordered_map<Library::AsyncFunction *, std::unique_ptr<Library::HostCallable>> async_functions;
    std::unordered_map<Library::Constant *, std::unique_ptr<Library::HostCallable>> constants;
    std::unordered_map<Library::AsyncConstant *, std::unique_ptr<Library::HostCallable>> async_constants;
};

LegacyHostCallableCache &host_cache(const Library *library) {
    static std::unordered_map<const Library *, LegacyHostCallableCache> caches;
    return caches[library];
}

} // namespace

Library::HostCallResult Library::HostCallResult::returned(const fiber::json::JsValue &value) {
    HostCallResult result;
    result.kind = HostCallResultKind::Return;
    result.value = value;
    return result;
}

Library::HostCallResult Library::HostCallResult::thrown(const fiber::json::JsValue &value) {
    HostCallResult result;
    result.kind = HostCallResultKind::Throw;
    result.value = value;
    return result;
}

Library::HostCallResult Library::HostCallResult::faulted(HostFault fault) {
    HostCallResult result;
    result.kind = HostCallResultKind::Fault;
    result.fault = fault;
    return result;
}

Library::HostCallResult Library::HostCallResult::pending() {
    HostCallResult result;
    result.kind = HostCallResultKind::Pending;
    return result;
}

const Library::HostCallable *Library::resolve_func(std::string_view name) const {
    return host_callable_for(const_cast<Library *>(this)->find_func(name));
}

const Library::HostCallable *Library::resolve_async_func(std::string_view name) const {
    return host_callable_for(const_cast<Library *>(this)->find_async_func(name));
}

const Library::HostCallable *Library::resolve_constant(std::string_view namespace_name, std::string_view key) const {
    return host_callable_for(const_cast<Library *>(this)->find_constant(namespace_name, key));
}

const Library::HostCallable *Library::resolve_async_constant(std::string_view namespace_name, std::string_view key) const {
    return host_callable_for(const_cast<Library *>(this)->find_async_constant(namespace_name, key));
}

const Library::HostCallable *Library::host_callable_for(Function *func) const {
    auto &cache = host_cache(this);
    return wrap_legacy_callable(func,
                                cache.functions,
                                HostCallable::Kind::SyncFunction,
                                legacy_function_thunk,
                                nullptr);
}

const Library::HostCallable *Library::host_callable_for(AsyncFunction *func) const {
    auto &cache = host_cache(this);
    return wrap_legacy_callable(func,
                                cache.async_functions,
                                HostCallable::Kind::AsyncFunction,
                                nullptr,
                                legacy_async_function_thunk);
}

const Library::HostCallable *Library::host_callable_for(Constant *constant) const {
    auto &cache = host_cache(this);
    return wrap_legacy_callable(constant,
                                cache.constants,
                                HostCallable::Kind::SyncConstant,
                                legacy_constant_thunk,
                                nullptr);
}

const Library::HostCallable *Library::host_callable_for(AsyncConstant *constant) const {
    auto &cache = host_cache(this);
    return wrap_legacy_callable(constant,
                                cache.async_constants,
                                HostCallable::Kind::AsyncConstant,
                                nullptr,
                                legacy_async_constant_thunk);
}

} // namespace fiber::script
