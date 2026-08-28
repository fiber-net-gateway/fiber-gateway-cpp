#ifndef FIBER_SCRIPT_AST_TEMPLATE_STRING_H
#define FIBER_SCRIPT_AST_TEMPLATE_STRING_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Expression.h"

namespace fiber::script::ast {

// A template literal `...${expr}...`. Mirrors io.fiber.net.script.ast.TemplateString:
// strings_.size() == expressions_.size() + 1, with the literal text chunk at
// strings_[i] preceding expressions_[i] and a trailing chunk at strings_.back().
// Compiled by lowering to a left-associative operator(Add) chain (see Compiler).
class TemplateString : public Expression {
public:
    TemplateString(std::int32_t start, std::int32_t end, std::vector<std::string> strings,
                   std::vector<std::unique_ptr<Expression>> expressions) :
        Expression(start, end), strings_(std::move(strings)), expressions_(std::move(expressions)) {}

    const std::vector<std::string> &strings() const { return strings_; }

    const std::vector<std::unique_ptr<Expression>> &expressions() const { return expressions_; }

private:
    std::vector<std::string> strings_;
    std::vector<std::unique_ptr<Expression>> expressions_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_TEMPLATE_STRING_H
