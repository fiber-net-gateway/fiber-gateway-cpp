#ifndef FIBER_SCRIPT_AST_DIRECTIVE_STATEMENT_H
#define FIBER_SCRIPT_AST_DIRECTIVE_STATEMENT_H

#include <memory>

#include "Statement.h"
#include "Identifier.h"
#include "../Library.h"

namespace fiber::script::ast {

class DirectiveStatement : public Statement {
public:
    DirectiveStatement(std::int32_t start,
                       std::int32_t end,
                       std::unique_ptr<Identifier> type,
                       std::unique_ptr<Identifier> name,
                       Library::DirectiveDef *def)
        : Statement(start, end), type_(std::move(type)), name_(std::move(name)), def_(def) {
    }

    const Identifier *type() const {
        return type_.get();
    }

    const Identifier *name() const {
        return name_.get();
    }

    Library::DirectiveDef *directive_def() const {
        return def_;
    }

private:
    std::unique_ptr<Identifier> type_;
    std::unique_ptr<Identifier> name_;
    Library::DirectiveDef *def_ = nullptr;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_DIRECTIVE_STATEMENT_H
