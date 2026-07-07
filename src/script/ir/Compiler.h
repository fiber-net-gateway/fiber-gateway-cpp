#ifndef FIBER_SCRIPT_IR_COMPILER_H
#define FIBER_SCRIPT_IR_COMPILER_H

#include "../ScriptLimits.h"
#include "../ast/Node.h"
#include "CompileResult.h"
#include "Compiled.h"

namespace fiber::script::ir {

class Compiler {
public:
    static CompileResult<Compiled> compile(const ast::Node &node, std::size_t max_depth = kDefaultScriptMaxDepth);
};

} // namespace fiber::script::ir

#endif // FIBER_SCRIPT_IR_COMPILER_H
