#ifndef FIBER_COMMON_NON_COPYABLE_H
#define FIBER_COMMON_NON_COPYABLE_H

#include "SpecialMember.h"

namespace fiber::common {

struct NonCopyable {
    // Deleted ops stay public (modernize-use-equals-delete): callers get a
    // clear "deleted function" error instead of a protected-access one.
    NonCopyable(const NonCopyable &) = delete;
    NonCopyable &operator=(const NonCopyable &) = delete;

protected:
    constexpr NonCopyable() = default;
    ~NonCopyable() = default;

    NonCopyable(NonCopyable &&) = default;
    NonCopyable &operator=(NonCopyable &&) = default;
};

} // namespace fiber::common

#endif // FIBER_COMMON_NON_COPYABLE_H
