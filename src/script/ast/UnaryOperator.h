#ifndef FIBER_SCRIPT_AST_UNARY_OPERATOR_H
#define FIBER_SCRIPT_AST_UNARY_OPERATOR_H

#include <cstdint>
#include <memory>

#include "Expression.h"
#include "Operator.h"

namespace fiber::script::ast {

class UnaryOperator : public Expression {
public:
    UnaryOperator(std::int32_t start,
                  std::int32_t end,
                  Operator op,
                  std::unique_ptr<Expression> operand)
        : Expression(start, end), op_(op), operand_(std::move(operand)) {
    }

    Operator op() const {
        return op_;
    }

    const Expression *operand() const {
        return operand_.get();
    }

    std::unique_ptr<Expression> take_operand() {
        return std::move(operand_);
    }

private:
    Operator op_ = Operator::Add;
    std::unique_ptr<Expression> operand_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_UNARY_OPERATOR_H
