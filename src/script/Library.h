#ifndef FIBER_SCRIPT_LIBRARY_H
#define FIBER_SCRIPT_LIBRARY_H

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "../common/json/JsNode.h"
#include "ExecutionContext.h"
#include "ScriptResult.h"
#include "async/AsyncTask.h"
#include "async/AsyncExecutionContext.h"

namespace fiber::script {

class ScriptRuntime;

class Library {
public:
    // Legacy hook kept while StdLibrary and existing tests migrate to ScriptResult.
    using FunctionResult = std::expected<fiber::json::JsValue, fiber::json::JsValue>;

    struct ScriptCallContext {
        ScriptRuntime *runtime = nullptr;
        const fiber::json::JsValue *root = nullptr;
        void *attach = nullptr;
        const fiber::json::JsValue *args = nullptr;
        std::uint32_t argc = 0;
        std::uint32_t flags = 0;

        [[nodiscard]] ScriptRuntime &runtime_ref() const noexcept;
        [[nodiscard]] const fiber::json::JsValue &root_value() const noexcept;
        [[nodiscard]] void *attach_ptr() const noexcept;
        [[nodiscard]] std::uint32_t arg_count() const noexcept;
        [[nodiscard]] const fiber::json::JsValue *arg(std::uint32_t index) const noexcept;
        [[nodiscard]] fiber::json::JsValue arg_or_undefined(std::uint32_t index) const noexcept;
    };

    struct HostCallFrame {
        ScriptRuntime *runtime = nullptr;
        const fiber::json::JsValue *root = nullptr;
        void *attach = nullptr;
        const fiber::json::JsValue *args = nullptr;
        std::uint32_t argc = 0;
        std::uint32_t flags = 0;
    };

    enum class HostFaultCode : std::uint16_t {
        None = 0,
        OutOfMemory,
        InvalidArgument,
        InvalidState,
        ServiceUnavailable,
        Timeout,
        Cancelled,
        Internal,
    };

    struct HostFault {
        HostFaultCode code = HostFaultCode::None;
        std::string_view name{};
        std::string_view message{};
        int status = 500;
        fiber::json::JsValue meta = fiber::json::JsValue::make_undefined();
    };

    enum class HostCallResultKind : std::uint8_t {
        Return = 0,
        Throw,
        Fault,
        Pending,
    };

    struct HostCallResult {
        HostCallResultKind kind = HostCallResultKind::Fault;
        fiber::json::JsValue value = fiber::json::JsValue::make_undefined();
        HostFault fault{};

        static HostCallResult returned(const fiber::json::JsValue &value) noexcept;
        static HostCallResult thrown(const fiber::json::JsValue &value) noexcept;
        static HostCallResult faulted(HostFault fault) noexcept;
        static HostCallResult pending() noexcept;
        static HostCallResult from_script_result(const ScriptResult &result) noexcept;
    };

    struct HostAsyncCompletion {
        void (*complete)(void *ctx, HostCallResult result) noexcept = nullptr;
        void *ctx = nullptr;
    };

    using HostSyncThunk = HostCallResult (*)(void *userdata, const HostCallFrame &frame) noexcept;
    using HostAsyncThunk = HostCallResult (*)(void *userdata, const HostCallFrame &frame,
                                              const HostAsyncCompletion &completion) noexcept;

    struct HostCallable {
        enum class Kind : std::uint8_t {
            SyncFunction = 0,
            AsyncFunction,
            SyncConstant,
            AsyncConstant,
        };

        Kind kind = Kind::SyncFunction;
        std::uint32_t flags = 0;
        void *userdata = nullptr;
        HostSyncThunk sync = nullptr;
        HostAsyncThunk async = nullptr;
        const char *debug_name = nullptr;
    };

    class Constant {
    public:
        virtual ~Constant() = default;
        virtual ScriptResult get(ScriptCallContext context) noexcept;
        // Legacy hook kept while concrete libraries migrate.
        virtual FunctionResult get(ExecutionContext &context);
    };

    class Function {
    public:
        virtual ~Function() = default;
        virtual ScriptResult call(ScriptCallContext context) noexcept;
        // Legacy hook kept while concrete libraries migrate.
        virtual FunctionResult call(ExecutionContext &context);
    };

    class AsyncConstant {
    public:
        virtual ~AsyncConstant() = default;
        virtual AsyncTask get(ScriptCallContext context) noexcept;
        // Legacy hook kept while concrete libraries migrate.
        virtual void get(AsyncExecutionContext &context);
    };

    class AsyncFunction {
    public:
        virtual ~AsyncFunction() = default;
        virtual AsyncTask call(ScriptCallContext context) noexcept;
        // Legacy hook kept while concrete libraries migrate.
        virtual void call(AsyncExecutionContext &context);
    };

    class DirectiveDef {
    public:
        virtual ~DirectiveDef() = default;
        virtual Function *find_func(std::string_view directive, std::string_view function) = 0;
        virtual AsyncFunction *find_async_func(std::string_view directive, std::string_view function) = 0;
    };

    virtual ~Library() = default;

    virtual void mark_root_prop(std::string_view prop_name) { (void) prop_name; }

    virtual Function *find_func(std::string_view name) = 0;
    virtual AsyncFunction *find_async_func(std::string_view name) = 0;
    virtual Constant *find_constant(std::string_view namespace_name, std::string_view key) = 0;
    virtual AsyncConstant *find_async_constant(std::string_view namespace_name, std::string_view key) = 0;
    virtual DirectiveDef *find_directive_def(std::string_view type, std::string_view name,
                                             const std::vector<fiber::json::JsValue> &literals) = 0;

    const HostCallable *resolve_func(std::string_view name) const;
    const HostCallable *resolve_async_func(std::string_view name) const;
    const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key) const;
    const HostCallable *resolve_async_constant(std::string_view namespace_name, std::string_view key) const;

    const HostCallable *host_callable_for(Function *func) const;
    const HostCallable *host_callable_for(AsyncFunction *func) const;
    const HostCallable *host_callable_for(Constant *constant) const;
    const HostCallable *host_callable_for(AsyncConstant *constant) const;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_LIBRARY_H
