#include "Library.h"

#include <exception>
#include <memory>
#include <unordered_map>

#include "../common/Assert.h"

namespace fiber::script {

namespace {

Library::ScriptCallContext script_context_from_frame(const Library::HostCallFrame &frame) noexcept {
    return Library::ScriptCallContext{frame.runtime, frame.root, frame.attach, frame.args, frame.argc, frame.flags};
}

ScriptResult script_result_from_legacy(Library::FunctionResult result) noexcept {
    if (result) {
        return ScriptResult::success(result.value());
    }
    return ScriptResult::exception(result.error());
}

Library::HostFaultCode host_fault_code_from_abort_reason(ScriptAbortReason reason) noexcept {
    switch (reason) {
        case ScriptAbortReason::OutOfMemory:
            return Library::HostFaultCode::OutOfMemory;
        case ScriptAbortReason::InvalidArgument:
        case ScriptAbortReason::TypeError:
        case ScriptAbortReason::IndexError:
        case ScriptAbortReason::DivisionByZero:
        case ScriptAbortReason::UnknownIdentifier:
            return Library::HostFaultCode::InvalidArgument;
        case ScriptAbortReason::InvalidState:
        case ScriptAbortReason::InvalidOpcode:
        case ScriptAbortReason::NoReturn:
            return Library::HostFaultCode::InvalidState;
        case ScriptAbortReason::Timeout:
            return Library::HostFaultCode::Timeout;
        case ScriptAbortReason::Cancelled:
            return Library::HostFaultCode::Cancelled;
        case ScriptAbortReason::HostFault:
        case ScriptAbortReason::Internal:
        case ScriptAbortReason::None:
            return Library::HostFaultCode::Internal;
    }
    return Library::HostFaultCode::Internal;
}

std::string_view abort_name(ScriptAbortReason reason) noexcept {
    switch (reason) {
        case ScriptAbortReason::OutOfMemory:
            return "EXEC_OUT_OF_MEMORY";
        case ScriptAbortReason::InvalidState:
            return "EXEC_INVALID_STATE";
        case ScriptAbortReason::InvalidArgument:
            return "EXEC_INVALID_ARGUMENT";
        case ScriptAbortReason::TypeError:
            return "EXEC_TYPE_ERROR";
        case ScriptAbortReason::IndexError:
            return "EXEC_INDEX_ERROR";
        case ScriptAbortReason::DivisionByZero:
            return "EXEC_DIVISION_BY_ZERO";
        case ScriptAbortReason::UnknownIdentifier:
            return "EXEC_UNKNOWN_IDENTIFIER";
        case ScriptAbortReason::InvalidOpcode:
            return "EXEC_INVALID_OPCODE";
        case ScriptAbortReason::NoReturn:
            return "EXEC_NO_RETURN";
        case ScriptAbortReason::HostFault:
            return "HOST_FAULT";
        case ScriptAbortReason::Timeout:
            return "EXEC_TIMEOUT";
        case ScriptAbortReason::Cancelled:
            return "EXEC_CANCELLED";
        case ScriptAbortReason::Internal:
        case ScriptAbortReason::None:
            return "EXEC_INTERNAL";
    }
    return "EXEC_INTERNAL";
}

std::string_view abort_message(ScriptAbortReason reason) noexcept {
    switch (reason) {
        case ScriptAbortReason::OutOfMemory:
            return "out of memory";
        case ScriptAbortReason::InvalidState:
            return "invalid script state";
        case ScriptAbortReason::InvalidArgument:
            return "invalid argument";
        case ScriptAbortReason::TypeError:
            return "type error";
        case ScriptAbortReason::IndexError:
            return "index error";
        case ScriptAbortReason::DivisionByZero:
            return "division by zero";
        case ScriptAbortReason::UnknownIdentifier:
            return "unknown identifier";
        case ScriptAbortReason::InvalidOpcode:
            return "invalid opcode";
        case ScriptAbortReason::NoReturn:
            return "script finished without return";
        case ScriptAbortReason::HostFault:
            return "host call failed";
        case ScriptAbortReason::Timeout:
            return "script timed out";
        case ScriptAbortReason::Cancelled:
            return "script cancelled";
        case ScriptAbortReason::Internal:
        case ScriptAbortReason::None:
            return "internal script error";
    }
    return "internal script error";
}

class LegacyExecutionContextAdapter final : public ExecutionContext {
public:
    explicit LegacyExecutionContextAdapter(const Library::HostCallFrame &frame) :
        context_(script_context_from_frame(frame)) {}

    explicit LegacyExecutionContextAdapter(Library::ScriptCallContext context) : context_(context) {}

    ScriptRuntime &runtime() override { return *context_.runtime; }

    const fiber::json::JsValue &root() const override { return *context_.root; }

    void *attach() const override { return context_.attach; }

    const fiber::json::JsValue &arg_value(std::size_t index) const override {
        if (!context_.args || index >= context_.argc) {
            return undefined_;
        }
        return context_.args[index];
    }

    std::size_t arg_count() const override { return context_.argc; }

private:
    Library::ScriptCallContext context_;
    fiber::json::JsValue undefined_ = fiber::json::JsValue::make_undefined();
};

class LegacyAsyncExecutionContextAdapter final : public AsyncExecutionContext {
public:
    LegacyAsyncExecutionContextAdapter(const Library::HostCallFrame &frame,
                                       const Library::HostAsyncCompletion &completion) :
        frame_(frame), completion_(completion) {}

    ScriptRuntime &runtime() override { return *frame_.runtime; }

    const fiber::json::JsValue &root() const override { return *frame_.root; }

    void *attach() const override { return frame_.attach; }

    const fiber::json::JsValue &arg_value(std::size_t index) const override {
        if (!frame_.args || index >= frame_.argc) {
            return undefined_;
        }
        return frame_.args[index];
    }

    std::size_t arg_count() const override { return frame_.argc; }

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

    bool done() const { return done_; }

    const Library::HostCallResult &result() const { return result_; }

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

template<typename T>
Library::HostCallResult make_internal_fault(T *object, std::string_view debug_name) {
    (void) object;
    Library::HostFault fault;
    fault.code = Library::HostFaultCode::Internal;
    fault.name = "HOST_INTERNAL";
    fault.message = debug_name.empty() ? "host callable threw a C++ exception" : debug_name;
    fault.status = 500;
    return Library::HostCallResult::faulted(fault);
}

Library::HostCallResult legacy_function_thunk(void *userdata, const Library::HostCallFrame &frame) noexcept {
    auto *func = static_cast<Library::Function *>(userdata);
    try {
        return Library::HostCallResult::from_script_result(func->call(script_context_from_frame(frame)));
    } catch (...) {
        return make_internal_fault(func, "legacy function threw");
    }
}

Library::HostCallResult legacy_constant_thunk(void *userdata, const Library::HostCallFrame &frame) noexcept {
    auto *constant = static_cast<Library::Constant *>(userdata);
    try {
        return Library::HostCallResult::from_script_result(constant->get(script_context_from_frame(frame)));
    } catch (...) {
        return make_internal_fault(constant, "legacy constant threw");
    }
}

Library::HostCallResult legacy_async_function_thunk(void *userdata, const Library::HostCallFrame &frame,
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

Library::HostCallResult legacy_async_constant_thunk(void *userdata, const Library::HostCallFrame &frame,
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

template<typename LegacyPtr>
const Library::HostCallable *
wrap_legacy_callable(LegacyPtr *legacy, std::unordered_map<LegacyPtr *, std::unique_ptr<Library::HostCallable>> &cache,
                     typename Library::HostCallable::Kind kind, Library::HostSyncThunk sync_thunk,
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

ScriptRuntime &Library::ScriptCallContext::runtime_ref() const noexcept {
    FIBER_ASSERT(runtime);
    return *runtime;
}

const fiber::json::JsValue &Library::ScriptCallContext::root_value() const noexcept {
    FIBER_ASSERT(root);
    return *root;
}

void *Library::ScriptCallContext::attach_ptr() const noexcept { return attach; }

std::uint32_t Library::ScriptCallContext::arg_count() const noexcept { return argc; }

const fiber::json::JsValue *Library::ScriptCallContext::arg(std::uint32_t index) const noexcept {
    if (!args || index >= argc) {
        return nullptr;
    }
    return args + index;
}

fiber::json::JsValue Library::ScriptCallContext::arg_or_undefined(std::uint32_t index) const noexcept {
    const fiber::json::JsValue *value = arg(index);
    if (!value) {
        return fiber::json::JsValue::make_undefined();
    }
    return *value;
}

Library::HostCallResult Library::HostCallResult::returned(const fiber::json::JsValue &value) noexcept {
    HostCallResult result;
    result.kind = HostCallResultKind::Return;
    result.value = value;
    return result;
}

Library::HostCallResult Library::HostCallResult::thrown(const fiber::json::JsValue &value) noexcept {
    HostCallResult result;
    result.kind = HostCallResultKind::Throw;
    result.value = value;
    return result;
}

Library::HostCallResult Library::HostCallResult::faulted(HostFault fault) noexcept {
    HostCallResult result;
    result.kind = HostCallResultKind::Fault;
    result.fault = fault;
    return result;
}

Library::HostCallResult Library::HostCallResult::pending() noexcept {
    HostCallResult result;
    result.kind = HostCallResultKind::Pending;
    return result;
}

Library::HostCallResult Library::HostCallResult::from_script_result(const ScriptResult &result) noexcept {
    if (result.is_success()) {
        return returned(result.value());
    }
    if (result.is_exception()) {
        return thrown(result.exception());
    }
    const ScriptAbort &abort = result.abort();
    HostFault fault;
    fault.code = host_fault_code_from_abort_reason(abort.reason);
    fault.name = abort_name(abort.reason);
    fault.message = abort_message(abort.reason);
    fault.status = 500;
    return faulted(fault);
}

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

Library::FunctionResult Library::Constant::get(ExecutionContext &context) {
    (void) context;
    return std::unexpected(fiber::json::JsValue::make_undefined());
}

ScriptResult Library::Constant::get(ScriptCallContext context) noexcept {
    try {
        LegacyExecutionContextAdapter legacy_context(context);
        return script_result_from_legacy(get(legacy_context));
    } catch (...) {
        FIBER_PANIC("C++ exception escaped script Constant");
    }
}

Library::FunctionResult Library::Function::call(ExecutionContext &context) {
    (void) context;
    return std::unexpected(fiber::json::JsValue::make_undefined());
}

ScriptResult Library::Function::call(ScriptCallContext context) noexcept {
    try {
        LegacyExecutionContextAdapter legacy_context(context);
        return script_result_from_legacy(call(legacy_context));
    } catch (...) {
        FIBER_PANIC("C++ exception escaped script Function");
    }
}

AsyncTask Library::AsyncConstant::get(ScriptCallContext context) noexcept {
    (void) context;
    return {};
}

void Library::AsyncConstant::get(AsyncExecutionContext &context) { (void) context; }

AsyncTask Library::AsyncFunction::call(ScriptCallContext context) noexcept {
    (void) context;
    return {};
}

void Library::AsyncFunction::call(AsyncExecutionContext &context) { (void) context; }

Library::FunctionMatchResult Library::resolve_func(std::string_view name, const FunctionMatchRequest &request) const {
    return const_cast<Library *>(this)->find_func(name, request);
}

Library::FunctionMatchResult Library::resolve_async_func(std::string_view name,
                                                         const FunctionMatchRequest &request) const {
    return const_cast<Library *>(this)->find_async_func(name, request);
}

const Library::HostCallable *Library::resolve_constant(std::string_view namespace_name, std::string_view key) const {
    return host_callable_for(const_cast<Library *>(this)->find_constant(namespace_name, key));
}

const Library::HostCallable *Library::resolve_async_constant(std::string_view namespace_name,
                                                             std::string_view key) const {
    return host_callable_for(const_cast<Library *>(this)->find_async_constant(namespace_name, key));
}

const Library::HostCallable *Library::host_callable_for(Function *func) const {
    auto &cache = host_cache(this);
    return wrap_legacy_callable(func, cache.functions, HostCallable::Kind::SyncFunction, legacy_function_thunk,
                                nullptr);
}

const Library::HostCallable *Library::host_callable_for(AsyncFunction *func) const {
    auto &cache = host_cache(this);
    return wrap_legacy_callable(func, cache.async_functions, HostCallable::Kind::AsyncFunction, nullptr,
                                legacy_async_function_thunk);
}

const Library::HostCallable *Library::host_callable_for(Constant *constant) const {
    auto &cache = host_cache(this);
    return wrap_legacy_callable(constant, cache.constants, HostCallable::Kind::SyncConstant, legacy_constant_thunk,
                                nullptr);
}

const Library::HostCallable *Library::host_callable_for(AsyncConstant *constant) const {
    auto &cache = host_cache(this);
    return wrap_legacy_callable(constant, cache.async_constants, HostCallable::Kind::AsyncConstant, nullptr,
                                legacy_async_constant_thunk);
}

} // namespace fiber::script
