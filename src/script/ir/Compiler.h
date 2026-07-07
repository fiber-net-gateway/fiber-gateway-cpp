#ifndef FIBER_SCRIPT_IR_COMPILER_H
#define FIBER_SCRIPT_IR_COMPILER_H

#include "../ast/Node.h"
#include "CompileResult.h"
#include "Compiled.h"

namespace fiber::script::ir {

class Compiler {
public:
    static CompileResult<Compiled> compile(const ast::Node &node);
};

} // namespace fiber::script::ir

#endif // FIBER_SCRIPT_IR_COMPILER_H
