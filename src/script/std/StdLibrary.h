#ifndef FIBER_SCRIPT_STD_LIBRARY_H
#define FIBER_SCRIPT_STD_LIBRARY_H

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "../Library.h"

namespace fiber::script::std_lib {

class StdLibrary : public Library {
public:
    struct ExtOps {
        void (*mark_root_prop)(void *ctx, std::string_view prop_name) = nullptr;
        FunctionMatchResult (*resolve_func)(void *ctx, std::string_view name,
                                            const FunctionMatchRequest &request) = nullptr;
        FunctionMatchResult (*resolve_async_func)(void *ctx, std::string_view name,
                                                  const FunctionMatchRequest &request) = nullptr;
        const HostCallable *(*resolve_constant)(void *ctx, std::string_view namespace_name,
                                                std::string_view key) = nullptr;
        const HostCallable *(*resolve_async_constant)(void *ctx, std::string_view namespace_name,
                                                      std::string_view key) = nullptr;
        DirectiveDef *(*resolve_directive_def)(void *ctx, std::string_view type, std::string_view name,
                                               const std::vector<fiber::script::JsValue> &literals) = nullptr;
    };

    static StdLibrary &instance();

    // Adds an ordered fallback resolver. Ops are copied, ctx is non-owning, and both resolver
    // calls and any ctx state changes must be serialized. Hosts must keep ctx and all userdata
    // returned through HostCallables alive for the compiled scripts that reference them.
    void add_ext_ops(void *ctx, ExtOps ops);

    void mark_root_prop(std::string_view prop_name) override;
    FunctionMatchResult resolve_func(std::string_view name, const FunctionMatchRequest &request) const override;
    FunctionMatchResult resolve_async_func(std::string_view name, const FunctionMatchRequest &request) const override;
    const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key) const override;
    const HostCallable *resolve_async_constant(std::string_view namespace_name, std::string_view key) const override;
    DirectiveDef *resolve_directive_def(std::string_view type, std::string_view name,
                                        const std::vector<fiber::script::JsValue> &literals) const override;

    void register_func(std::string_view name, FunctionSignature signature, Function function, void *userdata = nullptr,
                       const char *debug_name = nullptr);
    void register_func(std::string_view name, FunctionSignature signature, std::vector<fiber::script::JsValue> defaults,
                       Function function, void *userdata = nullptr, const char *debug_name = nullptr);
    void register_async_func(std::string_view name, FunctionSignature signature, AsyncFunction function,
                             void *userdata = nullptr, const char *debug_name = nullptr);
    void register_async_func(std::string_view name, FunctionSignature signature,
                             std::vector<fiber::script::JsValue> defaults, AsyncFunction function,
                             void *userdata = nullptr, const char *debug_name = nullptr);
    void register_constant(std::string_view name, Constant constant, void *userdata = nullptr,
                           const char *debug_name = nullptr);
    void register_async_constant(std::string_view name, AsyncConstant constant, void *userdata = nullptr,
                                 const char *debug_name = nullptr);

    // Public so hosts (e.g. lite_nginx) can own a StdLibrary instance and register
    // host-specific functions onto it via register_func/register_async_func, keeping the
    // process-wide instance() pure. The default standard functions are registered in the
    // constructor regardless of which instance is used.
    StdLibrary();

private:
    struct Extension {
        void *ctx = nullptr;
        ExtOps ops{};
    };

    struct FunctionEntry {
        FunctionSignature signature{};
        std::vector<fiber::script::JsValue> defaults;
        HostCallable callable;
    };

    std::unordered_map<std::string, std::deque<FunctionEntry>> functions_;
    std::unordered_map<std::string, std::deque<FunctionEntry>> async_functions_;
    std::unordered_map<std::string, HostCallable> constants_;
    std::unordered_map<std::string, HostCallable> async_constants_;
    std::vector<Extension> extensions_;
};

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_LIBRARY_H
