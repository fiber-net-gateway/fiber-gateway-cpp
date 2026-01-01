#ifndef FIBER_SCRIPT_PARSE_PARSE_ERROR_H
#define FIBER_SCRIPT_PARSE_PARSE_ERROR_H

#include <cstddef>
#include <string>

namespace fiber::script::parse {

struct ParseError {
    std::string message;
    std::size_t position = 0;
};

} // namespace fiber::script::parse

#endif // FIBER_SCRIPT_PARSE_PARSE_ERROR_H
