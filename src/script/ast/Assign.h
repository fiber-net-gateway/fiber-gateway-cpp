#ifndef FIBER_SCRIPT_AST_ASSIGN_H
#define FIBER_SCRIPT_AST_ASSIGN_H

#include <memory>

#include "Expression.h"
#include "MaybeLValue.h"

namespace fiber::script::ast {

class Assign : public Expression {
public:
    Assign(std::int32_t start,
           std::int32_t end,
           std::unique_ptr<MaybeLValue> left,
           std::unique_ptr<Expression> right)
        : Expression(start, end), left_(std::move(left)), right_(std::move(right)) {
    }

    const MaybeLValue *left() const {
        return left_.get();
    }

    const Expression *right() const {
        return right_.get();
    }

    std::unique_ptr<MaybeLValue> take_left() {
        return std::move(left_);
    }

    std::unique_ptr<Expression> take_right() {
        return std::move(right_);
    }

private:
    std::unique_ptr<MaybeLValue> left_;
    std::unique_ptr<Expression> right_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_ASSIGN_H
