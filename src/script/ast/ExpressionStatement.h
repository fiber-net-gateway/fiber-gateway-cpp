#ifndef FIBER_SCRIPT_AST_EXPRESSION_STATEMENT_H
#define FIBER_SCRIPT_AST_EXPRESSION_STATEMENT_H

#include <memory>

#include "Statement.h"
#include "Expression.h"

namespace fiber::script::ast {

class ExpressionStatement : public Statement {
public:
    ExpressionStatement(std::int32_t start, std::int32_t end, std::unique_ptr<Expression> expression)
        : Statement(start, end), expression_(std::move(expression)) {
    }

    const Expression *expression() const {
        return expression_.get();
    }

    std::unique_ptr<Expression> take_expression() {
        return std::move(expression_);
    }

private:
    std::unique_ptr<Expression> expression_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_EXPRESSION_STATEMENT_H
