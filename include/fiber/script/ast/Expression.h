#ifndef FIBER_SCRIPT_AST_EXPRESSION_H
#define FIBER_SCRIPT_AST_EXPRESSION_H

#include "Node.h"

namespace fiber::script::ast {

class Expression : public Node {
public:
    using Node::Node;
    ~Expression() override = default;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_EXPRESSION_H
