#ifndef FIBER_HTTP_HTTP1_CONNECTION_POOL_AFFINITY_H
#define FIBER_HTTP_HTTP1_CONNECTION_POOL_AFFINITY_H

#include <cstdint>
#include <type_traits>

namespace fiber::http {

// A fixed-size, non-secret application-assigned transport-profile partition
// for otherwise identical endpoints. Zero preserves legacy grouping. Use the
// same value for one immutable profile across all workers sharing a pool, and
// distinct nonzero values for profiles whose lifetimes overlap. Do not reuse a
// value until every connection and lease for its old profile has drained.
class Http1ConnectionPoolAffinity {
public:
    constexpr Http1ConnectionPoolAffinity() noexcept = default;
    explicit constexpr Http1ConnectionPoolAffinity(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

    friend constexpr bool operator==(Http1ConnectionPoolAffinity left,
                                     Http1ConnectionPoolAffinity right) noexcept = default;

private:
    std::uint64_t value_ = 0;
};

static_assert(sizeof(Http1ConnectionPoolAffinity) == sizeof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<Http1ConnectionPoolAffinity>);

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CONNECTION_POOL_AFFINITY_H
