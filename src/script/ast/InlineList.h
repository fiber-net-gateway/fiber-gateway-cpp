#ifndef FIBER_SCRIPT_AST_INLINE_LIST_H
#define FIBER_SCRIPT_AST_INLINE_LIST_H

#include <memory>
#include <vector>

#include "Expression.h"

namespace fiber::script::ast {

class InlineList : public Expression {
public:
    InlineList(std::int32_t start, std::int32_t end, std::vector<std::unique_ptr<Expression>> values)
        : Expression(start, end), values_(std::move(values)) {
    }

    const std::vector<std::unique_ptr<Expression>> &values() const {
        return values_;
    }

private:
    std::vector<std::unique_ptr<Expression>> values_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_INLINE_LIST_H
