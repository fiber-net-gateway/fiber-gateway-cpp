#ifndef FIBER_SCRIPT_AST_RETURN_STATEMENT_H
#define FIBER_SCRIPT_AST_RETURN_STATEMENT_H

#include <memory>

#include "Statement.h"
#include "Expression.h"

namespace fiber::script::ast {

class ReturnStatement : public Statement {
public:
    ReturnStatement(std::int32_t start, std::int32_t end, std::unique_ptr<Expression> value)
        : Statement(start, end), value_(std::move(value)) {
    }

    const Expression *value() const {
        return value_.get();
    }

    std::unique_ptr<Expression> take_value() {
        return std::move(value_);
    }

private:
    std::unique_ptr<Expression> value_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_RETURN_STATEMENT_H
