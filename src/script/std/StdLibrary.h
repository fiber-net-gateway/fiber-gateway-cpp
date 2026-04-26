#ifndef FIBER_SCRIPT_STD_LIBRARY_H
#define FIBER_SCRIPT_STD_LIBRARY_H

#include <string>
#include <unordered_map>
#include <vector>

#include "../Library.h"

namespace fiber::script::std_lib {

class StdLibrary : public Library {
public:
    static StdLibrary &instance();

    FunctionMatchResult find_func(std::string_view name, const FunctionMatchRequest &request) override;
    FunctionMatchResult find_async_func(std::string_view name, const FunctionMatchRequest &request) override;
    const HostCallable *find_constant(std::string_view namespace_name, std::string_view key) override;
    const HostCallable *find_async_constant(std::string_view namespace_name, std::string_view key) override;
    DirectiveDef *find_directive_def(std::string_view type, std::string_view name,
                                     const std::vector<fiber::json::JsValue> &literals) override;

    void register_func(std::string name, FunctionSignature signature, LegacyFunction *func);
    void register_func(std::string name, FunctionSignature signature, std::vector<fiber::json::JsValue> defaults,
                       LegacyFunction *func);
    void register_async_func(std::string name, FunctionSignature signature, LegacyAsyncFunction *func);
    void register_async_func(std::string name, FunctionSignature signature, std::vector<fiber::json::JsValue> defaults,
                             LegacyAsyncFunction *func);
    void register_constant(std::string name, LegacyConstant *constant);
    void register_async_constant(std::string name, LegacyAsyncConstant *constant);

private:
    struct FunctionEntry {
        FunctionSignature signature{};
        std::vector<fiber::json::JsValue> defaults;
        LegacyFunction *func = nullptr;
    };

    struct AsyncFunctionEntry {
        FunctionSignature signature{};
        std::vector<fiber::json::JsValue> defaults;
        LegacyAsyncFunction *func = nullptr;
    };

    StdLibrary();

    std::unordered_map<std::string, std::vector<FunctionEntry>> functions_;
    std::unordered_map<std::string, std::vector<AsyncFunctionEntry>> async_functions_;
    std::unordered_map<std::string, LegacyConstant *> constants_;
    std::unordered_map<std::string, LegacyAsyncConstant *> async_constants_;
};

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_LIBRARY_H
