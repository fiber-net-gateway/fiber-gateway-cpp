#ifndef FIBER_REGEX_SIMPLIFY_H
#define FIBER_REGEX_SIMPLIFY_H

#include "regex/Ast.h"

namespace fiber::regex::detail {

// Rewrites the tree to use only the operators the compiler knows: {n,m} becomes
// copies of the operand, empty and full classes become NoMatch and AnyChar, and
// adjacent repeats of one literal are merged. The copies share nodes, so the
// result is a DAG. `max` is the largest rune the encoding can express.
[[nodiscard]] Node *simplify(Ast &ast, Node *root, Rune max);

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_SIMPLIFY_H
