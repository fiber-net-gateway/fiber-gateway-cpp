#ifndef FIBER_SCRIPT_AST_BREAK_STATEMENT_H
#define FIBER_SCRIPT_AST_BREAK_STATEMENT_H

#include "Statement.h"

namespace fiber::script::ast {

class BreakStatement : public Statement {
public:
    BreakStatement(std::int32_t start, std::int32_t end)
        : Statement(start, end) {
    }
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_BREAK_STATEMENT_H
