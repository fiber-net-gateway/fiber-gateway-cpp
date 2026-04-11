#ifndef FIBER_SCRIPT_AST_FUNCTION_CALL_H
#define FIBER_SCRIPT_AST_FUNCTION_CALL_H

#include <memory>
#include <string>
#include <vector>

#include "Expression.h"
#include "../Library.h"

namespace fiber::script::ast {

class FunctionCall : public Expression {
public:
    FunctionCall(std::int32_t start,
                 std::int32_t end,
                 std::string name,
                 const Library::HostCallable *func,
                 const Library::HostCallable *async_func,
                 std::vector<std::unique_ptr<Expression>> args)
        : Expression(start, end),
          name_(std::move(name)),
          func_(func),
          async_func_(async_func),
          args_(std::move(args)) {
    }

    const std::string &name() const {
        return name_;
    }

    const Library::HostCallable *func() const {
        return func_;
    }

    const Library::HostCallable *async_func() const {
        return async_func_;
    }

    bool is_async() const {
        return async_func_ != nullptr;
    }

    const std::vector<std::unique_ptr<Expression>> &args() const {
        return args_;
    }

private:
    std::string name_;
    const Library::HostCallable *func_ = nullptr;
    const Library::HostCallable *async_func_ = nullptr;
    std::vector<std::unique_ptr<Expression>> args_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_FUNCTION_CALL_H
