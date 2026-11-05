#ifndef FIBER_REGEX_COMPILED_H
#define FIBER_REGEX_COMPILED_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "regex/Program.h"

namespace fiber::regex::detail {

struct CompiledName {
    std::string name{};
    std::uint32_t index = 0;
};

struct Compiled {
    Program program{};
    // Finds where a match begins once its end is known. Compiled up front, so that a
    // Regex never changes after it is built. Can be missing if it would have been
    // too large, in which case the Pike VM does that job.
    std::optional<Program> reverse{};
    std::uint32_t capture_count = 0;
    std::vector<CompiledName> names{};
};

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_COMPILED_H
