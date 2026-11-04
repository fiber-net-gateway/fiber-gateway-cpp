#ifndef FIBER_REGEX_PIKE_VM_H
#define FIBER_REGEX_PIKE_VM_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include <fiber/regex/Regex.h>

#include "regex/Program.h"
#include "regex/Search.h"
#include "regex/Sparse.h"

namespace fiber::regex::detail {

// Thread handle: an index into PikeWorkspace::threads.
using ThreadId = std::int32_t;
inline constexpr ThreadId kNoThread = -1;

// Reusable memory for pike_search. Lives in Scratch so that a warm worker does
// not allocate while matching.
struct PikeWorkspace {
    struct Thread {
        std::int32_t ref = 0;
        ThreadId next = kNoThread; // free list link
    };

    // Stack frame for walking empty arrows without recursion.
    struct AddState {
        std::uint32_t id = 0;
        ThreadId thread = kNoThread; // if set, makes it the current thread first
    };

    SparseArray<ThreadId> queue0{};
    SparseArray<ThreadId> queue1{};
    std::vector<AddState> stack{};
    std::vector<Thread> threads{};
    std::vector<std::size_t> captures{}; // ncapture entries per thread
    std::vector<std::size_t> match{};
};

// Thompson NFA simulation with capture tracking, in the style of Rob Pike's sam
// editor. Picks the same submatches a backtracking engine would, but time is
// bounded by text length times program size.
[[nodiscard]] MatchStatus pike_search(const Program &program, PikeWorkspace &workspace, const SearchRequest &request,
                                      WorkBudget &budget) noexcept;

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_PIKE_VM_H
