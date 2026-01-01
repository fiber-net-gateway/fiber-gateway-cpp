#ifndef FIBER_SCRIPT_AST_BLOCK_H
#define FIBER_SCRIPT_AST_BLOCK_H

#include <memory>
#include <vector>

#include "Statement.h"

namespace fiber::script::ast {

enum class BlockType : std::uint8_t {
    Script,
    TryBlock,
    CatchBlock,
    ForBlock,
    IfBlock,
    ElseBlock,
};

class Block : public Statement {
public:
    Block() = default;
    Block(std::int32_t start, std::int32_t end, BlockType type)
        : Statement(start, end), type_(type) {
    }

    BlockType type() const {
        return type_;
    }

    const std::vector<std::unique_ptr<Statement>> &statements() const {
        return statements_;
    }

    void add_statement(std::unique_ptr<Statement> stmt) {
        statements_.push_back(std::move(stmt));
    }

    void set_range(std::int32_t start, std::int32_t end) {
        start_ = start;
        end_ = end;
    }

private:
    BlockType type_ = BlockType::Script;
    std::vector<std::unique_ptr<Statement>> statements_;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_BLOCK_H
