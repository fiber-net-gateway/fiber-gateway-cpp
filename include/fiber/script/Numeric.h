#ifndef FIBER_SCRIPT_NUMERIC_H
#define FIBER_SCRIPT_NUMERIC_H

#include <cmath>
#include <cstdint>
#include <limits>

namespace fiber::script {

// Java's (long)double saturates at the int64 bounds and maps NaN to 0; a plain
// static_cast is undefined behavior for out-of-range magnitudes (on x86-64
// cvttsd2si silently yields INT64_MIN, and the optimizer may assume the cast
// never sees such inputs). Every double->int64 coercion in the runtime must go
// through this, never a bare static_cast.
inline std::int64_t double_to_int64_saturated(double d) noexcept {
    if (std::isnan(d)) {
        return 0;
    }
    if (d >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (d < static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(d);
}

} // namespace fiber::script

#endif // FIBER_SCRIPT_NUMERIC_H
