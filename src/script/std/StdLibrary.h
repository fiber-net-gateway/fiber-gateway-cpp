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
    Constant *find_constant(std::string_view namespace_name, std::string_view key) override;
    AsyncConstant *find_async_constant(std::string_view namespace_name, std::string_view key) override;
    DirectiveDef *find_directive_def(std::string_view type, std::string_view name,
                                     const std::vector<fiber::json::JsValue> &literals) override;

    void register_func(std::string name, FunctionSignature signature, Function *func);
    void register_func(std::string name, FunctionSignature signature, std::vector<fiber::json::JsValue> defaults,
                       Function *func);
    void register_async_func(std::string name, FunctionSignature signature, AsyncFunction *func);
    void register_async_func(std::string name, FunctionSignature signature, std::vector<fiber::json::JsValue> defaults,
                             AsyncFunction *func);
    void register_constant(std::string name, Constant *constant);
    void register_async_constant(std::string name, AsyncConstant *constant);

private:
    struct FunctionEntry {
        FunctionSignature signature{};
        std::vector<fiber::json::JsValue> defaults;
        Function *func = nullptr;
    };

    struct AsyncFunctionEntry {
        FunctionSignature signature{};
        std::vector<fiber::json::JsValue> defaults;
        AsyncFunction *func = nullptr;
    };

    StdLibrary();

    std::unordered_map<std::string, std::vector<FunctionEntry>> functions_;
    std::unordered_map<std::string, std::vector<AsyncFunctionEntry>> async_functions_;
    std::unordered_map<std::string, Constant *> constants_;
    std::unordered_map<std::string, AsyncConstant *> async_constants_;
};

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_LIBRARY_H
