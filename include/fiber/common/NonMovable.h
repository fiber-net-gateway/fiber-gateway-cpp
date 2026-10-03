#ifndef FIBER_COMMON_NON_MOVABLE_H
#define FIBER_COMMON_NON_MOVABLE_H

#include "SpecialMember.h"

namespace fiber::common {

struct NonMovable {
    // Deleted ops stay public (modernize-use-equals-delete): callers get a
    // clear "deleted function" error instead of a protected-access one.
    NonMovable(NonMovable &&) = delete;
    NonMovable &operator=(NonMovable &&) = delete;

protected:
    constexpr NonMovable() = default;
    ~NonMovable() = default;

    NonMovable(const NonMovable &) = default;
    NonMovable &operator=(const NonMovable &) = default;
};

} // namespace fiber::common

#endif // FIBER_COMMON_NON_MOVABLE_H
