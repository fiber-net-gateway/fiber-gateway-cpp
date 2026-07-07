#ifndef FIBER_SCRIPT_LIMITS_H
#define FIBER_SCRIPT_LIMITS_H

#include <cstddef>

namespace fiber::script {

inline constexpr std::size_t kDefaultScriptMaxDepth = 128;

struct ScriptLimits {
    std::size_t max_depth = kDefaultScriptMaxDepth;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_LIMITS_H
