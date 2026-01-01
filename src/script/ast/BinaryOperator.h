#ifndef FIBER_SCRIPT_AST_BINARY_OPERATOR_H
#define FIBER_SCRIPT_AST_BINARY_OPERATOR_H

#include <cstdint>
#include <memory>

#include "Expression.h"
#include "Operator.h"

namespace fiber::script::ast {

class BinaryOperator : public Expression {
public:
    BinaryOperator(std::int32_t start,
                   std::int32_t end,
                   Operator op,
                   std::unique_ptr<Expression> left,
                   std::unique_ptr<Expression> right)
        : Expression(start, end), op_(op), left_(std::move(left)), right_(std::move(right)) {
    }

    Operator op() const {
        return op_;
    }

    const Expression *left() const {
        return left_.get();
    }

    const Expression *right() const {
        return right_.get();
    }

    std::unique_ptr<Expression> take_left() {
        return std::move(left_);
    }

    std::unique_ptr<Expression> take_right() {
        return std::move(right_);
    }

private:
    Operator op_ = Operator::Add;
    std::unique_ptr<Expression> left_;
    std::unique_ptr<Expression> right_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_BINARY_OPERATOR_H
