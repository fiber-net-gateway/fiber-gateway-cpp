#ifndef FIBER_NACOS_NACOS_AUTH_H
#define FIBER_NACOS_NACOS_AUTH_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include <common/IoError.h>

namespace fiber::nacos {

enum class NacosAuthState : std::uint8_t {
    Pending,
    Ready,
    Unavailable,
    Stopped,
};

enum class NacosAuthErrorCode : std::uint8_t {
    None,
    Io,
    HttpStatus,
    ResponseTooLarge,
    InvalidJson,
    MissingAccessToken,
    InvalidTokenTtl,
    TokenExpired,
    Canceled,
};

struct NacosAuthError {
    NacosAuthErrorCode code = NacosAuthErrorCode::None;
    common::IoErr io_error = common::IoErr::None;
    int http_status = 0;
    std::size_t server_index = 0;
};

struct NacosAuthSnapshot {
    NacosAuthState state = NacosAuthState::Pending;
    std::string access_token;
    std::string username;
    bool global_admin = false;
    std::uint64_t generation = 0;
    std::chrono::steady_clock::time_point expires_at{};
    NacosAuthError last_error{};

    [[nodiscard]] bool ready() const noexcept { return state == NacosAuthState::Ready; }
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_NACOS_AUTH_H
