#ifndef FIBER_NET_HAPPY_EYEBALLS_H
#define FIBER_NET_HAPPY_EYEBALLS_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"

namespace fiber::net {

inline constexpr std::size_t kHappyEyeballsMaxAddresses = 16;
inline constexpr std::size_t kHappyEyeballsMaxConcurrentAttempts = 4;

enum class HappyEyeballsAddressPolicy : std::uint8_t {
    V6First,
    V4First,
    V6Only,
    V4Only,
};

struct HappyEyeballsOptions {
    // One deadline covers every TCP attempt. Individual attempts remain live until the first
    // success, failure, cancellation, destruction, loop shutdown, or this shared deadline.
    std::chrono::milliseconds total_timeout = std::chrono::milliseconds::max();
    std::chrono::milliseconds connection_attempt_delay{250};
    std::uint8_t max_concurrent_attempts = 2;
    std::uint8_t first_address_family_count = 1;
    HappyEyeballsAddressPolicy address_policy = HappyEyeballsAddressPolicy::V6First;
};

// Attempt details are indexed by the deterministic, interleaved candidate order. Entries with a
// bit absent from failed_mask have IoErr::None. input_indices maps each candidate back to the
// caller-provided address span.
struct HappyEyeballsConnectError {
    common::IoErr code = common::IoErr::Unknown;
    std::array<common::IoErr, kHappyEyeballsMaxAddresses> attempt_errors{};
    std::array<std::uint8_t, kHappyEyeballsMaxAddresses> input_indices{};
    std::uint16_t attempted_mask = 0;
    std::uint16_t failed_mask = 0;
    std::uint8_t candidate_count = 0;
    std::uint8_t attempted_count = 0;
};

} // namespace fiber::net

#endif // FIBER_NET_HAPPY_EYEBALLS_H
