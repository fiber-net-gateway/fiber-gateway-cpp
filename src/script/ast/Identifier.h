#ifndef FIBER_SCRIPT_AST_IDENTIFIER_H
#define FIBER_SCRIPT_AST_IDENTIFIER_H

#include <string>

#include "Expression.h"

namespace fiber::script::ast {

class Identifier : public Expression {
public:
    Identifier() = default;
    Identifier(std::int32_t start, std::int32_t end, std::string name)
        : Expression(start, end), name_(std::move(name)) {
    }

    const std::string &name() const {
        return name_;
    }

private:
    std::string name_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_IDENTIFIER_H
