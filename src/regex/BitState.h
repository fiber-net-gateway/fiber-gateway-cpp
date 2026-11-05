#ifndef FIBER_REGEX_BIT_STATE_H
#define FIBER_REGEX_BIT_STATE_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include <fiber/regex/Regex.h>

#include "regex/Program.h"
#include "regex/Search.h"

namespace fiber::regex::detail {

// Reusable memory for bit_state_search.
struct BitStateWorkspace {
    struct Job {
        std::int32_t id = 0; // negative: restore capture register of inst -id
        std::int32_t rle = 0;
        std::size_t pos = 0;
    };

    std::vector<std::uint64_t> visited{}; // one bit per (list, position)
    std::vector<std::size_t> capture{};
    std::vector<Job> jobs{};
};

// Backtracking search that never explores the same (list, position) twice, which
// keeps it linear in the text. Faster than the Pike VM on small programs and
// short texts: the caller must check Program::can_bit_state() and the text size.
[[nodiscard]] MatchStatus bit_state_search(const Program &program, BitStateWorkspace &workspace,
                                           const SearchRequest &request, WorkBudget &budget) noexcept;

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_BIT_STATE_H
