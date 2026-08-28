#ifndef FIBER_SCRIPT_IR_COMPILER_H
#define FIBER_SCRIPT_IR_COMPILER_H

#include <cstddef>

#include <fiber/script/ScriptLimits.h>
#include <fiber/script/ir/CompileResult.h>
#include <fiber/script/ir/Compiled.h>
#include "../ast/Expression.h"
#include "../ast/Node.h"

namespace fiber::script::ir {

class Compiler {
public:
    static CompileResult<Compiled> compile(const ast::Node &node, std::size_t max_depth = kDefaultScriptMaxDepth);

    // Compiles a single expression into a value-returning script: emits the expression then
    // END_RETURN so the evaluated value becomes the script result. compile()'s Expression branch
    // already performs compile_expression(expr) + emit_end_return, so this typed entry just
    // forwards to it (Expression& is a Node&).
    static CompileResult<Compiled> compile_expression(const ast::Expression &expr,
                                                      std::size_t max_depth = kDefaultScriptMaxDepth);
};

} // namespace fiber::script::ir

#endif // FIBER_SCRIPT_IR_COMPILER_H
