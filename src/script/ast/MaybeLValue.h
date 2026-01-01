#ifndef FIBER_SCRIPT_AST_MAYBE_LVALUE_H
#define FIBER_SCRIPT_AST_MAYBE_LVALUE_H

#include "Expression.h"

namespace fiber::script::ast {

class MaybeLValue : public Expression {
public:
    using Expression::Expression;

    bool is_lvalue() const {
        return lvalue_;
    }

    void mark_lvalue() {
        lvalue_ = true;
    }

private:
    bool lvalue_ = false;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_MAYBE_LVALUE_H
