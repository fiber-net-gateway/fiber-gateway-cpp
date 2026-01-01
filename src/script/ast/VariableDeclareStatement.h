#ifndef FIBER_SCRIPT_AST_VARIABLE_DECLARE_STATEMENT_H
#define FIBER_SCRIPT_AST_VARIABLE_DECLARE_STATEMENT_H

#include <memory>

#include "Statement.h"
#include "Identifier.h"
#include "Expression.h"

namespace fiber::script::ast {

class VariableDeclareStatement : public Statement {
public:
    VariableDeclareStatement(std::int32_t start,
                             std::int32_t end,
                             std::unique_ptr<Identifier> identifier,
                             std::unique_ptr<Expression> initializer)
        : Statement(start, end),
          identifier_(std::move(identifier)),
          initializer_(std::move(initializer)) {
    }

    const Identifier *identifier() const {
        return identifier_.get();
    }

    const Expression *initializer() const {
        return initializer_.get();
    }

private:
    std::unique_ptr<Identifier> identifier_;
    std::unique_ptr<Expression> initializer_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_VARIABLE_DECLARE_STATEMENT_H
