#ifndef FIBER_SCRIPT_AST_TRY_CATCH_STATEMENT_H
#define FIBER_SCRIPT_AST_TRY_CATCH_STATEMENT_H

#include <memory>

#include "Statement.h"
#include "Block.h"
#include "Identifier.h"

namespace fiber::script::ast {

class TryCatchStatement : public Statement {
public:
    TryCatchStatement(std::int32_t start,
                      std::int32_t end,
                      std::unique_ptr<Identifier> identifier,
                      std::unique_ptr<Block> try_block,
                      std::unique_ptr<Block> catch_block)
        : Statement(start, end),
          identifier_(std::move(identifier)),
          try_block_(std::move(try_block)),
          catch_block_(std::move(catch_block)) {
    }

    const Identifier *identifier() const {
        return identifier_.get();
    }

    const Block *try_block() const {
        return try_block_.get();
    }

    const Block *catch_block() const {
        return catch_block_.get();
    }

private:
    std::unique_ptr<Identifier> identifier_;
    std::unique_ptr<Block> try_block_;
    std::unique_ptr<Block> catch_block_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_TRY_CATCH_STATEMENT_H
