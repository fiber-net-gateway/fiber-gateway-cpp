#ifndef FIBER_SCRIPT_AST_EXPAND_ARR_ARG_H
#define FIBER_SCRIPT_AST_EXPAND_ARR_ARG_H

#include <memory>

#include "Expression.h"

namespace fiber::script::ast {

class ExpandArrArg : public Expression {
public:
    enum class Where : std::uint8_t {
        FuncCall,
        InitObj,
        InitArr,
    };

    ExpandArrArg(std::int32_t start, std::int32_t end, std::unique_ptr<Expression> value, Where where)
        : Expression(start, end), value_(std::move(value)), where_(where) {
    }

    const Expression *value() const {
        return value_.get();
    }

    Where where() const {
        return where_;
    }

private:
    std::unique_ptr<Expression> value_;
    Where where_ = Where::FuncCall;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_EXPAND_ARR_ARG_H
