#ifndef FIBER_SCRIPT_AST_INDEXER_H
#define FIBER_SCRIPT_AST_INDEXER_H

#include <memory>

#include "MaybeLValue.h"

namespace fiber::script::ast {

class Indexer : public MaybeLValue {
public:
    Indexer(std::int32_t start,
            std::int32_t end,
            std::unique_ptr<Expression> parent,
            std::unique_ptr<Expression> index)
        : MaybeLValue(start, end), parent_(std::move(parent)), index_(std::move(index)) {
    }

    const Expression *parent() const {
        return parent_.get();
    }

    const Expression *index() const {
        return index_.get();
    }

    std::unique_ptr<Expression> take_parent() {
        return std::move(parent_);
    }

    std::unique_ptr<Expression> take_index() {
        return std::move(index_);
    }

private:
    std::unique_ptr<Expression> parent_;
    std::unique_ptr<Expression> index_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_INDEXER_H
