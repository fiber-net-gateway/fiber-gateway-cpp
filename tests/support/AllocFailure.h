#ifndef FIBER_TESTS_SUPPORT_ALLOC_FAILURE_H
#define FIBER_TESTS_SUPPORT_ALLOC_FAILURE_H

#include <cstddef>
#include <limits>

// fiber_tests replaces the global operator new in GzipEncoderTest.cpp. The TSan runtime
// owns those symbols, so there is no replacement, and no failure injection, under TSan.
#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
#define FIBER_TEST_REPLACES_OPERATOR_NEW 0
#else
#define FIBER_TEST_REPLACES_OPERATOR_NEW 1
#endif

namespace fiber::test {

inline constexpr bool kAllocFailureAvailable = FIBER_TEST_REPLACES_OPERATOR_NEW != 0;

inline constexpr std::size_t kNoAllocFailure = std::numeric_limits<std::size_t>::max();
inline thread_local std::size_t alloc_successes_left = kNoAllocFailure;

// Called by the throwing operator new forms; true means fail this allocation.
[[nodiscard]] inline bool consume_alloc_attempt() noexcept {
    if (alloc_successes_left == kNoAllocFailure) {
        return false;
    }
    if (alloc_successes_left == 0) {
        return true;
    }
    --alloc_successes_left;
    return false;
}

// While alive, lets this thread's next `successes` throwing allocations through and then
// fails every later one, as real exhaustion would. Keep gtest assertions out of the scope:
// they allocate on failure.
class ScopedAllocFailure {
public:
    explicit ScopedAllocFailure(std::size_t successes) noexcept { alloc_successes_left = successes; }
    ~ScopedAllocFailure() { alloc_successes_left = kNoAllocFailure; }

    ScopedAllocFailure(const ScopedAllocFailure &) = delete;
    ScopedAllocFailure &operator=(const ScopedAllocFailure &) = delete;
    ScopedAllocFailure(ScopedAllocFailure &&) = delete;
    ScopedAllocFailure &operator=(ScopedAllocFailure &&) = delete;
};

} // namespace fiber::test

#endif // FIBER_TESTS_SUPPORT_ALLOC_FAILURE_H
