#ifndef FIBER_SCRIPT_LIBRARY_H
#define FIBER_SCRIPT_LIBRARY_H

#include <cstdint>
#include <string_view>
#include <vector>

#include "AsyncTask.h"
#include "JsValue.h"
#include "Runtime.h"
#include "ScriptResult.h"

namespace fiber::script {

class Library {
public:
    struct HostCallFrame {
        ScriptRuntime *runtime = nullptr;
        ConstValueHandle root = nullptr;
        void *attach = nullptr;
    };

    struct Arguments {
        ConstValueHandle args = nullptr;
        std::uint32_t argc = 0;
    };

    using Function = ScriptStatus (*)(void *userdata, const HostCallFrame &frame, const Arguments &arguments,
                                      ValueHandle out) noexcept;
    using AsyncFunction = AsyncTask (*)(void *userdata, const HostCallFrame &frame, const Arguments &arguments,
                                        ValueHandle out) noexcept;

    using Constant = ScriptStatus (*)(void *userdata, const HostCallFrame &frame, ValueHandle out) noexcept;
    using AsyncConstant = AsyncTask (*)(void *userdata, const HostCallFrame &frame, ValueHandle out) noexcept;

    struct HostCallable {
        enum class Kind : std::uint8_t {
            SyncFunction = 0,
            AsyncFunction,
            SyncConstant,
            AsyncConstant,
        };

        Kind kind = Kind::SyncFunction;
        void *userdata = nullptr;
        union {
            Function function = nullptr;
            AsyncFunction async_function;
            Constant constant;
            AsyncConstant async_constant;
        };
        const char *debug_name = nullptr;
    };

    struct FunctionSignature {
        std::uint16_t required_argc = 0;
        std::uint16_t fixed_argc = 0;
        bool variadic = true;
        const fiber::script::JsValue *defaults = nullptr;
        std::uint16_t default_count = 0;
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
        const fiber::script::JsValue *defaults_to_append = nullptr;
        std::uint16_t default_count = 0;

        static FunctionMatchResult not_found() noexcept;
        static FunctionMatchResult arity_mismatch() noexcept;
        static FunctionMatchResult ambiguous() noexcept;
        static FunctionMatchResult found(const HostCallable *callable, FunctionSignature signature,
                                         const fiber::script::JsValue *defaults, std::uint16_t default_count) noexcept;
    };

    class DirectiveDef {
    public:
        virtual ~DirectiveDef() = default;
        virtual FunctionMatchResult resolve_func(std::string_view directive, std::string_view function,
                                                 const FunctionMatchRequest &request, const Library &library) const = 0;
        virtual FunctionMatchResult resolve_async_func(std::string_view directive, std::string_view function,
                                                       const FunctionMatchRequest &request,
                                                       const Library &library) const = 0;
    };

    virtual ~Library() = default;

    virtual void mark_root_prop(std::string_view prop_name) { (void) prop_name; }

    virtual const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key) const = 0;
    virtual const HostCallable *resolve_async_constant(std::string_view namespace_name, std::string_view key) const = 0;
    virtual DirectiveDef *resolve_directive_def(std::string_view type, std::string_view name,
                                                const std::vector<fiber::script::JsValue> &literals) const = 0;

    virtual FunctionMatchResult resolve_func(std::string_view name, const FunctionMatchRequest &request) const = 0;
    virtual FunctionMatchResult resolve_async_func(std::string_view name,
                                                   const FunctionMatchRequest &request) const = 0;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_LIBRARY_H
