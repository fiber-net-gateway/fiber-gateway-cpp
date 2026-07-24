#ifndef FIBER_AI_SERVER_TOKEN_RATE_LIMIT_REMOTE_CLIENT_H
#define FIBER_AI_SERVER_TOKEN_RATE_LIMIT_REMOTE_CLIENT_H

#include "RateLimitHttpCodec.h"
#include "RateLimitShardRing.h"

#include <cstdint>
#include <expected>
#include <string>

#include <async/Task.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoopGroup.h>
#include <http/LocalHttp1ConnectionPoolSet.h>

namespace fiber::ai_server {

enum class RateLimitRemoteErrorCode : std::uint8_t {
    InvalidNode,
    PoolUnavailable,
    Connect,
    Send,
    Receive,
    ResponseTooLarge,
    HttpStatus,
    InvalidResponse,
};

struct RateLimitRemoteError {
    RateLimitRemoteErrorCode code = RateLimitRemoteErrorCode::InvalidResponse;
    common::IoErr io_error = common::IoErr::None;
    int status_code = 0;
};

class TokenRateLimitRemoteClient final : public common::NonCopyable, public common::NonMovable {
public:
    explicit TokenRateLimitRemoteClient(event::EventLoopGroup &workers) noexcept;
    ~TokenRateLimitRemoteClient();

    [[nodiscard]] bool init() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] async::Task<std::expected<RateLimitCheckResponse, RateLimitRemoteError>>
    check(const RateLimitNode &node, const RateLimitCheckRequest &request) noexcept;

    [[nodiscard]] async::Task<std::expected<RateLimitSettleResponse, RateLimitRemoteError>>
    settle(const RateLimitNode &node, const RateLimitSettleRequest &request) noexcept;

private:
    [[nodiscard]] async::Task<std::expected<std::string, RateLimitRemoteError>>
    post(const RateLimitNode &node, std::string_view path, std::string request_body) noexcept;

    http::LocalHttp1ConnectionPoolSet pool_;
    bool initialized_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_TOKEN_RATE_LIMIT_REMOTE_CLIENT_H
