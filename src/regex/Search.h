#ifndef FIBER_REGEX_SEARCH_H
#define FIBER_REGEX_SEARCH_H

#include <cstddef>
#include <span>
#include <string_view>

#include <fiber/regex/Regex.h>

namespace fiber::regex::detail {

// What every engine is asked to do. It searches context[begin, end), with the
// rest of context visible to empty-width assertions.
struct SearchRequest {
    std::string_view context{};
    std::size_t begin = 0;
    std::size_t end = 0;
    bool anchored = false; // only consider matches starting at begin
    bool longest = false; // prefer the longest match over the first
    bool full_match = false; // the match must end at end; implies anchored and longest
    bool earliest = false; // stop at the first match of any kind
    std::span<Span> captures{}; // receives $0 and the groups; may be empty
};

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_SEARCH_H
