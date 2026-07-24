#ifndef FIBER_AI_SERVER_TOKEN_RATE_LIMIT_COORDINATOR_H
#define FIBER_AI_SERVER_TOKEN_RATE_LIMIT_COORDINATOR_H

#include "RateLimitShardRing.h"
#include "TokenRateLimitRemoteClient.h"
#include "TokenRateLimitService.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <async/Spawn.h>
#include <async/Task.h>
#include <async/WaitGroup.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>

namespace fiber::ai_server {

enum class RateLimitCoordinatorErrorCode : std::uint8_t {
    RingUnavailable,
    OwnerRequestFailed,
    OwnerRuleUnavailable,
    InvalidOwnerResponse,
    Stopping,
};

struct RateLimitCoordinatorError {
    RateLimitCoordinatorErrorCode code = RateLimitCoordinatorErrorCode::RingUnavailable;
    RateLimitRemoteError remote;
};

struct CoordinatedRateLimitCheck {
    TokenRateLimitCheckResult result;
    std::optional<RateLimitNode> owner;
};

class TokenRateLimitCoordinator final : public common::NonCopyable, public common::NonMovable {
public:
    TokenRateLimitCoordinator(TokenRateLimitService &local_service, RateLimitShardRing &ring,
                              TokenRateLimitRemoteClient &remote_client) noexcept :
        local_service_(&local_service), ring_(&ring), remote_client_(&remote_client) {}
    ~TokenRateLimitCoordinator();

    [[nodiscard]] bool init() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] async::Task<std::expected<CoordinatedRateLimitCheck, RateLimitCoordinatorError>>
    check(std::string_view user_id, std::string_view model_name, std::int64_t now_millis) noexcept;

    void settle(RateLimitNode owner, std::string_view user_id, std::string_view model_name, TokenRateLimitTicket ticket,
                std::int64_t tokens, bool count_usage, std::int64_t now_millis) noexcept;

    [[nodiscard]] async::Task<std::expected<RateLimitSettleResponse, RateLimitCoordinatorError>>
    settle_and_wait(RateLimitNode owner, std::string_view user_id, std::string_view model_name,
                    TokenRateLimitTicket ticket, std::int64_t tokens, bool count_usage,
                    std::int64_t now_millis) noexcept;

private:
    [[nodiscard]] async::DetachedTask settle_remote(RateLimitNode owner, std::string user_id, std::string model_name,
                                                    TokenRateLimitTicket ticket, std::int64_t tokens,
                                                    bool count_usage) noexcept;

    TokenRateLimitService *local_service_ = nullptr;
    RateLimitShardRing *ring_ = nullptr;
    TokenRateLimitRemoteClient *remote_client_ = nullptr;
    async::WaitGroup remote_settles_;
    std::atomic<bool> stopping_{false};
    bool initialized_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_TOKEN_RATE_LIMIT_COORDINATOR_H
