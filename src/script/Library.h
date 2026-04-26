#ifndef FIBER_SCRIPT_LIBRARY_H
#define FIBER_SCRIPT_LIBRARY_H

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "../common/json/JsNode.h"
#include "ExecutionContext.h"
#include "ScriptResult.h"
#include "async/AsyncExecutionContext.h"
#include "async/AsyncTask.h"

namespace fiber::script {

class ScriptRuntime;

class Library {
public:
    using FunctionResult = std::expected<fiber::json::JsValue, fiber::json::JsValue>;

    struct HostCallFrame {
        ScriptRuntime *runtime = nullptr;
        const fiber::json::JsValue *root = nullptr;
        void *attach = nullptr;
    };

    struct Arguments {
        const fiber::json::JsValue *args = nullptr;
        std::uint32_t argc = 0;
    };

    using Function = ScriptResult (*)(void *userdata, const HostCallFrame &frame, const Arguments &arguments) noexcept;
    using AsyncFunction = AsyncTask (*)(void *userdata, const HostCallFrame &frame,
                                        const Arguments &arguments) noexcept;

    using Constant = ScriptResult (*)(void *userdata, const HostCallFrame &frame) noexcept;
    using AsyncConstant = AsyncTask (*)(void *userdata, const HostCallFrame &frame) noexcept;

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
        Function function = nullptr;
        AsyncFunction async_function = nullptr;
        Constant constant = nullptr;
        AsyncConstant async_constant = nullptr;
        const char *debug_name = nullptr;
    };

    struct FunctionSignature {
        std::uint16_t required_argc = 0;
        std::uint16_t fixed_argc = 0;
        bool variadic = true;
        const fiber::json::JsValue *defaults = nullptr;
        std::uint16_t default_count = 0;
    };

    template<typename FC>
        requires(std::is_same_v<FC, Function> || std::is_same_v<FC, AsyncFunction>)
    struct HostFunction {
        FC fc = nullptr;
        std::string name;
        void *userdata = nullptr;
        FunctionSignature fs;
    };

    struct FunctionMatchRequest {
        std::uint16_t known_argc = 0;
        bool has_spread = false;
        bool spread_argc_unknown = false;
    };

    enum class FunctionMatchStatus : std::uint8_t {
        Found = 0,
        NotFound,
        ArityMismatch,
        Ambiguous,
    };

    struct FunctionMatchResult {
        FunctionMatchStatus status = FunctionMatchStatus::NotFound;
        const HostCallable *callable = nullptr;
        FunctionSignature signature{};
        const fiber::json::JsValue *defaults_to_append = nullptr;
        std::uint16_t default_count = 0;

        static FunctionMatchResult not_found() noexcept;
        static FunctionMatchResult arity_mismatch() noexcept;
        static FunctionMatchResult ambiguous() noexcept;
        static FunctionMatchResult found(const HostCallable *callable, FunctionSignature signature,
                                         const fiber::json::JsValue *defaults, std::uint16_t default_count) noexcept;
    };

    class LegacyConstant {
    public:
        virtual ~LegacyConstant() = default;
        virtual FunctionResult get(ExecutionContext &context);
    };

    class LegacyFunction {
    public:
        virtual ~LegacyFunction() = default;
        virtual FunctionResult call(ExecutionContext &context);
    };

    class LegacyAsyncConstant {
    public:
        virtual ~LegacyAsyncConstant() = default;
        virtual void get(AsyncExecutionContext &context);
    };

    class LegacyAsyncFunction {
    public:
        virtual ~LegacyAsyncFunction() = default;
        virtual void call(AsyncExecutionContext &context);
    };


    class DirectiveDef {
    public:
        virtual ~DirectiveDef() = default;
        virtual FunctionMatchResult find_func(std::string_view directive, std::string_view function,
                                              const FunctionMatchRequest &request, const Library &library) = 0;
        virtual FunctionMatchResult find_async_func(std::string_view directive, std::string_view function,
                                                    const FunctionMatchRequest &request, const Library &library) = 0;
    };

    virtual ~Library() = default;

    virtual void mark_root_prop(std::string_view prop_name) { (void) prop_name; }

    const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key) const;
    const HostCallable *resolve_async_constant(std::string_view namespace_name, std::string_view key) const;
    virtual DirectiveDef *find_directive_def(std::string_view type, std::string_view name,
                                             const std::vector<fiber::json::JsValue> &literals) = 0;

    FunctionMatchResult resolve_func(std::string_view name, const FunctionMatchRequest &request) const;
    FunctionMatchResult resolve_async_func(std::string_view name, const FunctionMatchRequest &request) const;

    virtual LegacyConstant *find_constant(std::string_view namespace_name, std::string_view key) = 0;
    virtual LegacyAsyncConstant *find_async_constant(std::string_view namespace_name, std::string_view key) = 0;
    virtual FunctionMatchResult find_func(std::string_view name, const FunctionMatchRequest &request) = 0;
    virtual FunctionMatchResult find_async_func(std::string_view name, const FunctionMatchRequest &request) = 0;

    const HostCallable *host_callable_for(LegacyFunction *func) const;
    const HostCallable *host_callable_for(LegacyAsyncFunction *func) const;
    const HostCallable *host_callable_for(LegacyConstant *constant) const;
    const HostCallable *host_callable_for(LegacyAsyncConstant *constant) const;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_LIBRARY_H
