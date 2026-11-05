#ifndef FIBER_SCRIPT_STD_REGEX_CACHE_H
#define FIBER_SCRIPT_STD_REGEX_CACHE_H

#include <string_view>

#include <fiber/regex/Regex.h>
#include <fiber/regex/Scratch.h>

namespace fiber::script::std_lib {

// Scripts pass patterns as plain strings on every call, so compiling each time
// would dominate the match. Returns the compiled pattern from a per-thread LRU, or
// nullptr if the pattern does not compile (which is remembered too). The pointer
// stays valid until the next call.
[[nodiscard]] const regex::Regex *cached_regex(std::string_view pattern);

// Per-thread matching memory.
[[nodiscard]] regex::Scratch &regex_scratch();

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_REGEX_CACHE_H
