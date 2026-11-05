#ifndef FIBER_REGEX_ONE_PASS_H
#define FIBER_REGEX_ONE_PASS_H

#include <cstddef>

#include <fiber/regex/Regex.h>

#include "regex/Program.h"
#include "regex/Search.h"

namespace fiber::regex::detail {

// Largest number of submatches, $0 included, that OnePass can report.
inline constexpr std::size_t kMaxOnePassCapture = 5;

// Anchored search for one-pass programs: at each byte at most one alternative can
// proceed, so no thread bookkeeping is needed. The request must be anchored,
// Program::is_one_pass() must hold and at most kMaxOnePassCapture may be asked for.
[[nodiscard]] MatchStatus one_pass_search(const Program &program, const SearchRequest &request,
                                          WorkBudget &budget) noexcept;

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_ONE_PASS_H
