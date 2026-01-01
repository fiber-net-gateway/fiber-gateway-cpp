#ifndef FIBER_SCRIPT_AST_STATEMENT_H
#define FIBER_SCRIPT_AST_STATEMENT_H

#include "Node.h"

namespace fiber::script::ast {

class Statement : public Node {
public:
    using Node::Node;
    ~Statement() override = default;
};

} // namespace fiber::script::ast

#endif // FIBER_SCRIPT_AST_STATEMENT_H
