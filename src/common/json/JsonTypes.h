#ifndef FIBER_JSONTYPES_H
#define FIBER_JSONTYPES_H

#include <cstddef>

namespace fiber::json {

struct ParseError {
    const char *message = nullptr;
    std::size_t offset = 0;
};

} // namespace fiber::json

#endif // FIBER_JSONTYPES_H
