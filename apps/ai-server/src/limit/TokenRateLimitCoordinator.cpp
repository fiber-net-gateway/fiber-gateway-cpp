#include "TokenRateLimitCoordinator.h"

#include <utility>

#include <async/Spawn.h>
#include <common/Assert.h>
#include <log/Log.h>

namespace fiber::ai_server {
namespace {

DEFINE_LOGGER(LOG_RATE_LIMIT, "ai_server.rate_limit");

RateLimitCoordinatorError coordinator_error(RateLimitCoordinatorErrorCode code,
                                            RateLimitRemoteError remote = {}) noexcept {
    return RateLimitCoordinatorError{.code = code, .remote = remote};
}

TokenRateLimitCheckResult from_http_response(const RateLimitCheckResponse &value) noexcept {
    TokenRateLimitCheckResult output{
            .rule_matched = value.rule_matched,
            .allowed = value.allowed,
            .used_tokens = value.used_tokens,
            .max_tokens = value.max_tokens,
            .recover_at_millis = value.recover_at_millis,
    };
    if (value.ticket) {
        output.has_ticket = true;
        output.ticket = TokenRateLimitTicket{
                .generation = value.ticket->generation,
                .window_start_millis = value.ticket->window_start_millis,
        };
    }
    return output;
}

} // namespace

TokenRateLimitCoordinator::~TokenRateLimitCoordinator() {
    FIBER_ASSERT(!initialized_);
    FIBER_ASSERT(remote_settles_.empty());
}

bool TokenRateLimitCoordinator::init() noexcept {
    if (initialized_) {
        return true;
    }
    if (!remote_client_->init()) {
        return false;
    }
    stopping_.store(false, std::memory_order_release);
    initialized_ = true;
    return true;
}

async::Task<void> TokenRateLimitCoordinator::shutdown() noexcept {
    if (!initialized_) {
        co_return;
    }
    stopping_.store(true, std::memory_order_release);
    co_await remote_settles_.join();
    co_await remote_client_->shutdown();
    initialized_ = false;
}

async::Task<std::expected<CoordinatedRateLimitCheck, RateLimitCoordinatorError>>
TokenRateLimitCoordinator::check(std::string_view user_id, std::string_view model_name,
                                 std::int64_t now_millis) noexcept {
    if (!local_service_->has_rule(model_name)) {
        co_return CoordinatedRateLimitCheck{
                .result = TokenRateLimitCheckResult{},
        };
    }
    const auto owner = ring_->locate(user_id, model_name);
    if (!owner) {
        co_return std::unexpected(coordinator_error(RateLimitCoordinatorErrorCode::RingUnavailable));
    }

    TokenRateLimitCheckResult result;
    if (owner->local) {
        result = local_service_->check(user_id, model_name, now_millis);
    } else {
        auto response = co_await remote_client_->check(*owner, RateLimitCheckRequest{
                                                                       .user_id = user_id,
                                                                       .model_name = model_name,
                                                               });
        if (!response) {
            co_return std::unexpected(
                    coordinator_error(RateLimitCoordinatorErrorCode::OwnerRequestFailed, response.error()));
        }
        result = from_http_response(*response);
    }
    if (!result.rule_matched) {
        co_return std::unexpected(coordinator_error(RateLimitCoordinatorErrorCode::OwnerRuleUnavailable));
    }
    if (result.allowed && !result.has_ticket) {
        co_return std::unexpected(coordinator_error(RateLimitCoordinatorErrorCode::InvalidOwnerResponse));
    }
    co_return CoordinatedRateLimitCheck{
            .result = result,
            .owner = *owner,
    };
}

void TokenRateLimitCoordinator::settle(RateLimitNode owner, std::string_view user_id, std::string_view model_name,
                                       TokenRateLimitTicket ticket, std::int64_t tokens, bool count_usage,
                                       std::int64_t now_millis) noexcept {
    if (!initialized_ || stopping_.load(std::memory_order_acquire)) {
        return;
    }
    if (owner.local) {
        (void) local_service_->settle(user_id, model_name, ticket, tokens, count_usage, now_millis);
        return;
    }
    remote_settles_.add();
    async::spawn([this, owner = std::move(owner), user_id = std::string(user_id), model_name = std::string(model_name),
                  ticket, tokens, count_usage]() mutable {
        return settle_remote(std::move(owner), std::move(user_id), std::move(model_name), ticket, tokens, count_usage);
    });
}

async::Task<std::expected<RateLimitSettleResponse, RateLimitCoordinatorError>>
TokenRateLimitCoordinator::settle_and_wait(RateLimitNode owner, std::string_view user_id, std::string_view model_name,
                                           TokenRateLimitTicket ticket, std::int64_t tokens, bool count_usage,
                                           std::int64_t now_millis) noexcept {
    if (!initialized_ || stopping_.load(std::memory_order_acquire)) {
        co_return std::unexpected(coordinator_error(RateLimitCoordinatorErrorCode::Stopping));
    }
    if (owner.local) {
        co_return to_http_response(
                local_service_->settle(user_id, model_name, ticket, tokens, count_usage, now_millis));
    }
    auto response = co_await remote_client_->settle(
            owner, RateLimitSettleRequest{
                           .user_id = user_id,
                           .model_name = model_name,
                           .ticket =
                                   RateLimitTicketPayload{
                                           .generation = ticket.generation,
                                           .window_start_millis = ticket.window_start_millis,
                                   },
                           .tokens = tokens,
                           .count_usage = count_usage,
                   });
    if (!response) {
        co_return std::unexpected(
                coordinator_error(RateLimitCoordinatorErrorCode::OwnerRequestFailed, response.error()));
    }
    co_return *response;
}

async::DetachedTask TokenRateLimitCoordinator::settle_remote(RateLimitNode owner, std::string user_id,
                                                             std::string model_name, TokenRateLimitTicket ticket,
                                                             std::int64_t tokens, bool count_usage) noexcept {
    FIBER_ASSERT(!owner.local);
    auto result = co_await settle_and_wait(owner, user_id, model_name, ticket, tokens, count_usage, 0);
    if (!result) {
        LOG(LOG_RATE_LIMIT, WARN) << "remote token rate limit settle failed owner=" << log::quoted(owner.node_id)
                                  << " error=" << static_cast<int>(result.error().code)
                                  << " io_error=" << common::io_err_name(result.error().remote.io_error);
    } else if (result->stale) {
        LOG(LOG_RATE_LIMIT, WARN) << "stale token rate limit settle ignored owner=" << log::quoted(owner.node_id);
    }
    remote_settles_.done();
}

} // namespace fiber::ai_server
