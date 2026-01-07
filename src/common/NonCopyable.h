#ifndef FIBER_COMMON_NON_COPYABLE_H
#define FIBER_COMMON_NON_COPYABLE_H

namespace fiber::common {

struct NonCopyable {
protected:
    constexpr NonCopyable() = default;
    ~NonCopyable() = default;

    NonCopyable(const NonCopyable &) = delete;
    NonCopyable &operator=(const NonCopyable &) = delete;

    NonCopyable(NonCopyable &&) = default;
    NonCopyable &operator=(NonCopyable &&) = default;
};

} // namespace fiber::common

#endif // FIBER_COMMON_NON_COPYABLE_H
