#ifndef FIBER_SCRIPT_AST_IF_STATEMENT_H
#define FIBER_SCRIPT_AST_IF_STATEMENT_H

#include <memory>

#include "Expression.h"
#include "Statement.h"

namespace fiber::script::ast {

class IfStatement : public Statement {
public:
    IfStatement(std::int32_t start,
                std::int32_t end,
                std::unique_ptr<Expression> condition,
                std::unique_ptr<Statement> then_branch,
                std::unique_ptr<Statement> else_branch)
        : Statement(start, end),
          condition_(std::move(condition)),
          then_branch_(std::move(then_branch)),
          else_branch_(std::move(else_branch)) {
    }

    const Expression *condition() const {
        return condition_.get();
    }

    const Statement *then_branch() const {
        return then_branch_.get();
    }

    const Statement *else_branch() const {
        return else_branch_.get();
    }

    std::unique_ptr<Expression> take_condition() {
        return std::move(condition_);
    }

    std::unique_ptr<Statement> take_then_branch() {
        return std::move(then_branch_);
    }

    std::unique_ptr<Statement> take_else_branch() {
        return std::move(else_branch_);
    }

private:
    std::unique_ptr<Expression> condition_;
    std::unique_ptr<Statement> then_branch_;
    std::unique_ptr<Statement> else_branch_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_IF_STATEMENT_H
