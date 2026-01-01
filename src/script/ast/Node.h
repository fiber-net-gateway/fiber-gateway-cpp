#ifndef FIBER_SCRIPT_AST_NODE_H
#define FIBER_SCRIPT_AST_NODE_H

#include <cstdint>

namespace fiber::script::ast {

class Node {
public:
    Node() = default;
    Node(std::int32_t start, std::int32_t end) : start_(start), end_(end) {
    }
    virtual ~Node() = default;

    std::int32_t start_pos() const {
        return start_;
    }

    std::int32_t end_pos() const {
        return end_;
    }

protected:
    std::int32_t start_ = 0;
    std::int32_t end_ = 0;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_NODE_H
