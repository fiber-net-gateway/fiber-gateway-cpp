#ifndef FIBER_SCRIPT_AST_TERNARY_H
#define FIBER_SCRIPT_AST_TERNARY_H

#include <memory>

#include "Expression.h"

namespace fiber::script::ast {

class Ternary : public Expression {
public:
    Ternary(std::int32_t start,
            std::int32_t end,
            std::unique_ptr<Expression> test,
            std::unique_ptr<Expression> if_true,
            std::unique_ptr<Expression> if_false)
        : Expression(start, end),
          test_(std::move(test)),
          if_true_(std::move(if_true)),
          if_false_(std::move(if_false)) {
    }

    const Expression *test() const {
        return test_.get();
    }

    const Expression *if_true() const {
        return if_true_.get();
    }

    const Expression *if_false() const {
        return if_false_.get();
    }

private:
    std::unique_ptr<Expression> test_;
    std::unique_ptr<Expression> if_true_;
    std::unique_ptr<Expression> if_false_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_TERNARY_H
