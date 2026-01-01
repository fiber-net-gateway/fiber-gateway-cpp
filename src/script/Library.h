#ifndef FIBER_SCRIPT_LIBRARY_H
#define FIBER_SCRIPT_LIBRARY_H

#include <string_view>
#include <vector>

#include "ExecutionContext.h"
#include "async/Task.h"

namespace fiber::script {

class Library {
public:
    class Constant {
    public:
        virtual ~Constant() = default;
        virtual fiber::json::JsValue get(ExecutionContext &context) = 0;
    };

    class Function {
    public:
        virtual ~Function() = default;
        virtual fiber::json::JsValue call(ExecutionContext &context) = 0;
    };

    class AsyncConstant {
    public:
        virtual ~AsyncConstant() = default;
        virtual async::Task<fiber::json::JsValue> get(ExecutionContext &context) = 0;
    };

    class AsyncFunction {
    public:
        virtual ~AsyncFunction() = default;
        virtual async::Task<fiber::json::JsValue> call(ExecutionContext &context) = 0;
    };

    class DirectiveDef {
    public:
        virtual ~DirectiveDef() = default;
        virtual Function *find_func(std::string_view directive, std::string_view function) = 0;
        virtual AsyncFunction *find_async_func(std::string_view directive, std::string_view function) = 0;
    };

    virtual ~Library() = default;

    virtual void mark_root_prop(std::string_view prop_name) {
        (void)prop_name;
    }

    virtual Function *find_func(std::string_view name) = 0;
    virtual AsyncFunction *find_async_func(std::string_view name) = 0;
    virtual Constant *find_constant(std::string_view namespace_name, std::string_view key) = 0;
    virtual AsyncConstant *find_async_constant(std::string_view namespace_name, std::string_view key) = 0;
    virtual DirectiveDef *find_directive_def(std::string_view type,
                                             std::string_view name,
                                             const std::vector<fiber::json::JsValue> &literals) = 0;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_LIBRARY_H
