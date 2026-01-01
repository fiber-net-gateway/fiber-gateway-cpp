#ifndef FIBER_SCRIPT_PARSE_OPTIMISER_H
#define FIBER_SCRIPT_PARSE_OPTIMISER_H

#include <memory>

#include "../ast/Node.h"

namespace fiber::script::parse {

std::unique_ptr<ast::Node> optimise(std::unique_ptr<ast::Node> node);

} // namespace fiber::script::parse

#endif // FIBER_SCRIPT_PARSE_OPTIMISER_H
