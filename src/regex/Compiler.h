#ifndef FIBER_REGEX_COMPILER_H
#define FIBER_REGEX_COMPILER_H

#include <cstdint>
#include <expected>

#include <fiber/regex/Regex.h>

#include "regex/Ast.h"
#include "regex/Program.h"

namespace fiber::regex::detail {

// Compiles a simplified tree. A reversed program runs backward over the text and
// locates the start of a match whose end is known.
[[nodiscard]] std::expected<Program, Error> compile_program(Ast &ast, Node *root, const Options &options,
                                                            bool reversed);

// Compiles the alternation of a RegexSet, whose branches end in HaveMatch nodes.
[[nodiscard]] std::expected<Program, Error> compile_set(Ast &ast, Node *root, const Options &options);

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_COMPILER_H
