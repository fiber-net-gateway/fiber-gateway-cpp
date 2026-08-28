#ifndef FIBER_SCRIPT_AST_FUNCTION_CALL_H
#define FIBER_SCRIPT_AST_FUNCTION_CALL_H

#include <memory>
#include <string>
#include <vector>

#include <fiber/script/Library.h>
#include "Expression.h"

namespace fiber::script::ast {

class FunctionCall : public Expression {
public:
    FunctionCall(std::int32_t start, std::int32_t end, std::string name, const Library::HostCallable *func,
                 const Library::HostCallable *async_func, std::vector<std::unique_ptr<Expression>> args,
                 std::vector<fiber::script::JsValue> default_args = {}) :
        Expression(start, end), name_(std::move(name)), func_(func), async_func_(async_func), args_(std::move(args)),
        default_args_(std::move(default_args)) {}

    const std::string &name() const { return name_; }

    const Library::HostCallable *func() const { return func_; }

    const Library::HostCallable *async_func() const { return async_func_; }

    bool is_async() const { return async_func_ != nullptr; }

    const std::vector<std::unique_ptr<Expression>> &args() const { return args_; }

    const std::vector<fiber::script::JsValue> &default_args() const { return default_args_; }

private:
    std::string name_;
    const Library::HostCallable *func_ = nullptr;
    const Library::HostCallable *async_func_ = nullptr;
    std::vector<std::unique_ptr<Expression>> args_;
    std::vector<fiber::script::JsValue> default_args_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_FUNCTION_CALL_H
