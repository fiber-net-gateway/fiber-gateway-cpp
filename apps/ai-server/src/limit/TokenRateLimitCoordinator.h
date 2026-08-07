#ifndef FIBER_AI_SERVER_TOKEN_RATE_LIMIT_COORDINATOR_H
#define FIBER_AI_SERVER_TOKEN_RATE_LIMIT_COORDINATOR_H

#include "RateLimitShardRing.h"
#include "TokenRateLimitRemoteClient.h"
#include "TokenRateLimitService.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/cat/MessageTrace.h>
#include <fiber/cat/Transaction.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/BufPool.h>

namespace fiber::ai_server {

class AiServerCatRequest;

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

enum class RateLimitSettleOutcome : std::uint8_t {
    Applied,
    Stale,
    Error,
};

struct RateLimitSettleCompletion {
    void *context = nullptr;
    void (*callback)(void *, RateLimitSettleOutcome) noexcept = nullptr;

    void notify(RateLimitSettleOutcome outcome) const noexcept {
        if (callback != nullptr) {
            callback(context, outcome);
        }
    }
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
    check(std::string_view user_id, const CompiledModelRoute &model, std::int64_t now_millis,
          AiServerCatRequest *cat_request = nullptr) noexcept;

    void settle(RateLimitNode owner, std::string_view user_id, std::string_view model_name, TokenRateLimitTicket ticket,
                std::int64_t tokens, bool count_usage, std::int64_t now_millis, RateLimitSettleCompletion completion,
                AiServerCatRequest *cat_request = nullptr) noexcept;

    [[nodiscard]] async::Task<std::expected<RateLimitSettleResponse, RateLimitCoordinatorError>>
    settle_and_wait(RateLimitNode owner, std::string_view user_id, std::string_view model_name,
                    TokenRateLimitTicket ticket, std::int64_t tokens, bool count_usage,
                    std::int64_t now_millis) noexcept;

private:
    [[nodiscard]] async::Task<std::expected<RateLimitSettleResponse, RateLimitCoordinatorError>>
    settle_remote_and_wait(const RateLimitNode &owner, std::string_view user_id, std::string_view model_name,
                           TokenRateLimitTicket ticket, std::int64_t tokens, bool count_usage,
                           const cat::MessageTraceContext *cat_context = nullptr,
                           std::string_view trace_state = {}) noexcept;

    [[nodiscard]] async::DetachedTask settle_remote(RateLimitNode owner, std::string user_id, std::string model_name,
                                                    TokenRateLimitTicket ticket, std::int64_t tokens, bool count_usage,
                                                    RateLimitSettleCompletion completion,
                                                    std::unique_ptr<mem::BufPool> cat_context_pool,
                                                    std::optional<cat::MessageTraceContext> cat_context,
                                                    std::string trace_state) noexcept;

    TokenRateLimitService *local_service_ = nullptr;
    RateLimitShardRing *ring_ = nullptr;
    TokenRateLimitRemoteClient *remote_client_ = nullptr;
    async::WaitGroup remote_settles_;
    std::atomic<bool> stopping_{false};
    bool initialized_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_TOKEN_RATE_LIMIT_COORDINATOR_H
