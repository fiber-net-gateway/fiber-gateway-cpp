#ifndef FIBER_COMMON_NON_MOVABLE_H
#define FIBER_COMMON_NON_MOVABLE_H

namespace fiber::common {

struct NonMovable {
protected:
    constexpr NonMovable() = default;
    ~NonMovable() = default;

    NonMovable(const NonMovable &) = default;
    NonMovable &operator=(const NonMovable &) = default;

    NonMovable(NonMovable &&) = delete;
    NonMovable &operator=(NonMovable &&) = delete;
};

} // namespace fiber::common

#endif // FIBER_COMMON_NON_MOVABLE_H
