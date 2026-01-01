#ifndef FIBER_SCRIPT_AST_LOGIC_RELATIONAL_EXPRESSION_H
#define FIBER_SCRIPT_AST_LOGIC_RELATIONAL_EXPRESSION_H

#include <memory>

#include "Expression.h"
#include "Operator.h"

namespace fiber::script::ast {

class LogicRelationalExpression : public Expression {
public:
    LogicRelationalExpression(std::int32_t start,
                              std::int32_t end,
                              std::unique_ptr<Expression> left,
                              Operator op,
                              std::unique_ptr<Expression> right)
        : Expression(start, end), left_(std::move(left)), op_(op), right_(std::move(right)) {
    }

    const Expression *left() const {
        return left_.get();
    }

    Operator op() const {
        return op_;
    }

    const Expression *right() const {
        return right_.get();
    }

private:
    std::unique_ptr<Expression> left_;
    Operator op_ = Operator::And;
    std::unique_ptr<Expression> right_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_LOGIC_RELATIONAL_EXPRESSION_H
