#ifndef FIBER_SCRIPT_AST_CONSTANT_VAL_H
#define FIBER_SCRIPT_AST_CONSTANT_VAL_H

#include <string>

#include "Expression.h"
#include "../Library.h"

namespace fiber::script::ast {

class ConstantVal : public Expression {
public:
    ConstantVal(std::int32_t start,
                std::int32_t end,
                std::string name,
                Library::Constant *constant,
                Library::AsyncConstant *async_constant)
        : Expression(start, end),
          name_(std::move(name)),
          constant_(constant),
          async_constant_(async_constant) {
    }

    const std::string &name() const {
        return name_;
    }

    bool is_async() const {
        return async_constant_ != nullptr;
    }

    Library::Constant *constant() const {
        return constant_;
    }

    Library::AsyncConstant *async_constant() const {
        return async_constant_;
    }

private:
    std::string name_;
    Library::Constant *constant_ = nullptr;
    Library::AsyncConstant *async_constant_ = nullptr;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_CONSTANT_VAL_H
