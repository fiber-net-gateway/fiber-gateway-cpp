#ifndef FIBER_JSONTYPES_H
#define FIBER_JSONTYPES_H

#include <cstddef>

namespace fiber::json {

struct ParseError {
    const char *message = nullptr;
    std::size_t offset = 0;
    // Set when the decoder itself ran out of memory, which says nothing about the input.
    bool out_of_memory = false;
};

} // namespace fiber::json

#endif // FIBER_JSONTYPES_H
