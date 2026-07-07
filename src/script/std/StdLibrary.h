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
    static StdLibrary &instance();

    FunctionMatchResult resolve_func(std::string_view name, const FunctionMatchRequest &request) const override;
    FunctionMatchResult resolve_async_func(std::string_view name, const FunctionMatchRequest &request) const override;
    const HostCallable *resolve_constant(std::string_view namespace_name, std::string_view key) const override;
    const HostCallable *resolve_async_constant(std::string_view namespace_name, std::string_view key) const override;
    DirectiveDef *resolve_directive_def(std::string_view type, std::string_view name,
                                        const std::vector<fiber::script::JsValue> &literals) const override;

    void register_func(std::string name, FunctionSignature signature, Function function, void *userdata = nullptr,
                       const char *debug_name = nullptr);
    void register_func(std::string name, FunctionSignature signature, std::vector<fiber::script::JsValue> defaults,
                       Function function, void *userdata = nullptr, const char *debug_name = nullptr);
    void register_async_func(std::string name, FunctionSignature signature, AsyncFunction function,
                             void *userdata = nullptr, const char *debug_name = nullptr);
    void register_async_func(std::string name, FunctionSignature signature, std::vector<fiber::script::JsValue> defaults,
                             AsyncFunction function, void *userdata = nullptr, const char *debug_name = nullptr);
    void register_constant(std::string name, Constant constant, void *userdata = nullptr,
                           const char *debug_name = nullptr);
    void register_async_constant(std::string name, AsyncConstant constant, void *userdata = nullptr,
                                 const char *debug_name = nullptr);

private:
    struct FunctionEntry {
        FunctionSignature signature{};
        std::vector<fiber::script::JsValue> defaults;
        HostCallable callable;
    };

    StdLibrary();

    std::unordered_map<std::string, std::deque<FunctionEntry>> functions_;
    std::unordered_map<std::string, std::deque<FunctionEntry>> async_functions_;
    std::unordered_map<std::string, HostCallable> constants_;
    std::unordered_map<std::string, HostCallable> async_constants_;
};

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_LIBRARY_H
