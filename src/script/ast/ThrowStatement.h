#ifndef FIBER_SCRIPT_AST_THROW_STATEMENT_H
#define FIBER_SCRIPT_AST_THROW_STATEMENT_H

#include <memory>

#include "Statement.h"
#include "Expression.h"

namespace fiber::script::ast {

class ThrowStatement : public Statement {
public:
    ThrowStatement(std::int32_t start, std::int32_t end, std::unique_ptr<Expression> value)
        : Statement(start, end), value_(std::move(value)) {
    }

    const Expression *value() const {
        return value_.get();
    }

private:
    std::unique_ptr<Expression> value_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_THROW_STATEMENT_H
