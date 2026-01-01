#ifndef FIBER_SCRIPT_AST_FOREACH_STATEMENT_H
#define FIBER_SCRIPT_AST_FOREACH_STATEMENT_H

#include <memory>

#include "Statement.h"
#include "Block.h"
#include "Identifier.h"
#include "Expression.h"

namespace fiber::script::ast {

class ForeachStatement : public Statement {
public:
    ForeachStatement(std::int32_t start,
                     std::int32_t end,
                     std::unique_ptr<Identifier> key,
                     std::unique_ptr<Identifier> value,
                     std::unique_ptr<Expression> collection,
                     std::unique_ptr<Block> block)
        : Statement(start, end),
          key_(std::move(key)),
          value_(std::move(value)),
          collection_(std::move(collection)),
          block_(std::move(block)) {
    }

    const Identifier *key() const {
        return key_.get();
    }

    const Identifier *value() const {
        return value_.get();
    }

    const Expression *collection() const {
        return collection_.get();
    }

    const Block *block() const {
        return block_.get();
    }

private:
    std::unique_ptr<Identifier> key_;
    std::unique_ptr<Identifier> value_;
    std::unique_ptr<Expression> collection_;
    std::unique_ptr<Block> block_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_FOREACH_STATEMENT_H
