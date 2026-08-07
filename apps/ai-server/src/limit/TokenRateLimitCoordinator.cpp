#include "TokenRateLimitCoordinator.h"

#include "../observability/AiServerCatRequest.h"
#include "../observability/AiServerLogCategories.h"

#include <array>
#include <charconv>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/cat/Status.h>
#include <fiber/common/Assert.h>
#include <fiber/log/Log.h>

namespace fiber::ai_server {
namespace {

DEFINE_LOGGER(LOG_RATE_LIMIT, kAiServerRateLimitLogger);

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
                .rule_revision = value.ticket->rule_revision,
                .generation = value.ticket->generation,
                .window_start_millis = value.ticket->window_start_millis,
        };
    }
    return output;
}

template<typename Message, typename Integer>
void add_cat_integer(Message *message, std::string_view key, Integer value) noexcept {
    if (!message) {
        return;
    }
    std::array<char, std::numeric_limits<Integer>::digits10 + 3> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec == std::errc{}) {
        (void) message->add_data(
                key, std::string_view(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())));
    }
}

template<typename Message>
void add_check_context(Message *message, std::string_view model_name, const RateLimitNode *owner,
                       const CompiledModelRateLimitRule &rule) noexcept {
    if (!message) {
        return;
    }
    (void) message->add_data("model", model_name);
    if (owner) {
        (void) message->add_data("owner", owner->node_id);
    }
    add_cat_integer(message, "rule_revision", rule.revision);
}

template<typename Message>
void add_check_result(Message *message, const TokenRateLimitCheckResult &result) noexcept {
    add_cat_integer(message, "used", result.used_tokens);
    add_cat_integer(message, "max", result.max_tokens);
    add_cat_integer(message, "recover_at", result.recover_at_millis);
}

template<typename Message>
void add_settle_context(Message *message, std::string_view model_name, const RateLimitNode &owner,
                        TokenRateLimitTicket ticket, std::int64_t tokens, bool count_usage) noexcept {
    if (!message) {
        return;
    }
    (void) message->add_data("model", model_name);
    (void) message->add_data("owner", owner.node_id);
    (void) message->add_data("count_usage", count_usage ? std::string_view("true") : std::string_view("false"));
    add_cat_integer(message, "rule_revision", ticket.rule_revision);
    add_cat_integer(message, "tokens", tokens);
}

template<typename Message>
void complete_cat_record(Message *message, std::string_view result, bool success,
                         std::string_view reason = {}) noexcept {
    if (!message) {
        return;
    }
    (void) message->add_data("result", result);
    if (!reason.empty()) {
        (void) message->add_data("reason", reason);
    }
    (void) message->complete(success ? cat::status::Success : cat::status::Fail);
}

std::optional<cat::Event> start_rate_limit_event(AiServerCatRequest *cat_request, std::string_view type,
                                                 std::string_view name) noexcept {
    if (!cat_request) {
        return std::nullopt;
    }
    auto event = cat_request->start_event(type, name);
    if (!event) {
        return std::nullopt;
    }
    return std::move(*event);
}

std::optional<cat::Transaction> start_rate_limit_transaction(AiServerCatRequest *cat_request, std::string_view type,
                                                             std::string_view name) noexcept {
    if (!cat_request) {
        return std::nullopt;
    }
    auto transaction = cat_request->start_transaction(type, name);
    if (!transaction) {
        return std::nullopt;
    }
    return std::move(*transaction);
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
TokenRateLimitCoordinator::check(std::string_view user_id, const CompiledModelRoute &model, std::int64_t now_millis,
                                 AiServerCatRequest *cat_request) noexcept {
    if (!model.rate_limit) {
        co_return CoordinatedRateLimitCheck{
                .result = TokenRateLimitCheckResult{},
        };
    }
    const std::string_view model_name = model.model_name;
    const CompiledModelRateLimitRule &rule = *model.rate_limit;
    const auto owner = ring_->locate(user_id, model_name);
    if (!owner) {
        auto event = start_rate_limit_event(cat_request, "RateLimit.Check", "unavailable");
        add_check_context(event ? &*event : nullptr, model_name, nullptr, rule);
        complete_cat_record(event ? &*event : nullptr, "error", false, "ring_unavailable");
        co_return std::unexpected(coordinator_error(RateLimitCoordinatorErrorCode::RingUnavailable));
    }

    TokenRateLimitCheckResult result;
    std::optional<cat::Event> local_event;
    std::optional<cat::Transaction> remote_transaction;
    if (owner->local) {
        local_event = start_rate_limit_event(cat_request, "RateLimit.Check", "local");
        add_check_context(local_event ? &*local_event : nullptr, model_name, &*owner, rule);
        result = local_service_->check(user_id, model_name, rule, now_millis);
    } else {
        remote_transaction = start_rate_limit_transaction(cat_request, "RateLimit.Check", "remote");
        add_check_context(remote_transaction ? &*remote_transaction : nullptr, model_name, &*owner, rule);
        std::optional<cat::MessageTraceContext> remote_context;
        if (cat_request) {
            auto created = cat_request->create_remote_context(remote_transaction ? &*remote_transaction : nullptr);
            if (created) {
                remote_context.emplace(std::move(*created));
            }
        }
        auto response = co_await remote_client_->check(*owner,
                                                       RateLimitCheckRequest{
                                                               .user_id = user_id,
                                                               .model_name = model_name,
                                                               .rule_revision = rule.revision,
                                                               .window_duration_millis = rule.window_duration_millis,
                                                               .max_tokens_per_window = rule.max_tokens_per_window,
                                                       },
                                                       remote_context ? &*remote_context : nullptr,
                                                       cat_request ? cat_request->trace_state() : std::string_view{});
        if (!response) {
            complete_cat_record(remote_transaction ? &*remote_transaction : nullptr, "error", false,
                                "owner_request_failed");
            co_return std::unexpected(
                    coordinator_error(RateLimitCoordinatorErrorCode::OwnerRequestFailed, response.error()));
        }
        result = from_http_response(*response);
    }
    auto finish_check = [&](std::string_view record_result, bool success, std::string_view reason = {}) noexcept {
        cat::Event *event = local_event ? &*local_event : nullptr;
        cat::Transaction *transaction = remote_transaction ? &*remote_transaction : nullptr;
        add_check_result(event, result);
        add_check_result(transaction, result);
        complete_cat_record(event, record_result, success, reason);
        complete_cat_record(transaction, record_result, success, reason);
    };
    if (!result.rule_matched) {
        finish_check("error", false, "owner_rule_unavailable");
        co_return std::unexpected(coordinator_error(RateLimitCoordinatorErrorCode::OwnerRuleUnavailable));
    }
    if (result.allowed && (!result.has_ticket || result.ticket.rule_revision != rule.revision)) {
        finish_check("error", false, "invalid_owner_response");
        co_return std::unexpected(coordinator_error(RateLimitCoordinatorErrorCode::InvalidOwnerResponse));
    }
    finish_check(result.allowed ? std::string_view("allow") : std::string_view("deny"), true);
    co_return CoordinatedRateLimitCheck{
            .result = result,
            .owner = *owner,
    };
}

void TokenRateLimitCoordinator::settle(RateLimitNode owner, std::string_view user_id, std::string_view model_name,
                                       TokenRateLimitTicket ticket, std::int64_t tokens, bool count_usage,
                                       std::int64_t now_millis, RateLimitSettleCompletion completion,
                                       AiServerCatRequest *cat_request) noexcept {
    std::optional<cat::Event> local_event;
    std::optional<cat::Transaction> remote_transaction;
    if (owner.local) {
        local_event = start_rate_limit_event(cat_request, "RateLimit.Settle", "local");
        add_settle_context(local_event ? &*local_event : nullptr, model_name, owner, ticket, tokens, count_usage);
    } else {
        remote_transaction = start_rate_limit_transaction(cat_request, "RateLimit.Settle", "remote");
        add_settle_context(remote_transaction ? &*remote_transaction : nullptr, model_name, owner, ticket, tokens,
                           count_usage);
    }
    if (!initialized_ || stopping_.load(std::memory_order_acquire)) {
        LOG(LOG_RATE_LIMIT, WARN) << "token rate limit settle rejected while stopping owner="
                                  << log::quoted(owner.node_id);
        complete_cat_record(local_event ? &*local_event : nullptr, "error", false, "stopping");
        complete_cat_record(remote_transaction ? &*remote_transaction : nullptr, "error", false, "stopping");
        completion.notify(RateLimitSettleOutcome::Error);
        return;
    }
    if (owner.local) {
        const TokenRateLimitSettleResult result =
                local_service_->settle(user_id, model_name, ticket, tokens, count_usage, now_millis);
        if (!result.applied) {
            LOG(LOG_RATE_LIMIT, WARN) << "stale local token rate limit settle ignored owner="
                                      << log::quoted(owner.node_id);
        }
        complete_cat_record(local_event ? &*local_event : nullptr,
                            result.applied ? std::string_view("applied") : std::string_view("stale"), true);
        completion.notify(result.applied ? RateLimitSettleOutcome::Applied : RateLimitSettleOutcome::Stale);
        return;
    }
    std::unique_ptr<mem::BufPool> remote_context_pool(new (std::nothrow) mem::BufPool);
    std::optional<cat::MessageTraceContext> remote_context;
    std::string trace_state;
    if (cat_request && remote_context_pool) {
        auto created = cat_request->create_remote_context(*remote_context_pool,
                                                          remote_transaction ? &*remote_transaction : nullptr);
        if (created) {
            remote_context.emplace(std::move(*created));
            trace_state.assign(cat_request->trace_state());
        }
    }
    // CAT traces and transactions are bound to the request EventLoop and pool. The remote settle is detached, so
    // finish its local record before handing the pool-backed propagation view to the asynchronous operation.
    complete_cat_record(remote_transaction ? &*remote_transaction : nullptr, "scheduled", true);
    remote_transaction.reset();
    remote_settles_.add();
    async::spawn([this, owner = std::move(owner), user_id = std::string(user_id), model_name = std::string(model_name),
                  ticket, tokens, count_usage, completion, cat_context_pool = std::move(remote_context_pool),
                  cat_context = std::move(remote_context), trace_state = std::move(trace_state)]() mutable {
        return settle_remote(std::move(owner), std::move(user_id), std::move(model_name), ticket, tokens, count_usage,
                             completion, std::move(cat_context_pool), std::move(cat_context), std::move(trace_state));
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
    co_return co_await settle_remote_and_wait(owner, user_id, model_name, ticket, tokens, count_usage);
}

async::Task<std::expected<RateLimitSettleResponse, RateLimitCoordinatorError>>
TokenRateLimitCoordinator::settle_remote_and_wait(const RateLimitNode &owner, std::string_view user_id,
                                                  std::string_view model_name, TokenRateLimitTicket ticket,
                                                  std::int64_t tokens, bool count_usage,
                                                  const cat::MessageTraceContext *cat_context,
                                                  std::string_view trace_state) noexcept {
    FIBER_ASSERT(!owner.local);
    auto response =
            co_await remote_client_->settle(owner,
                                            RateLimitSettleRequest{
                                                    .user_id = user_id,
                                                    .model_name = model_name,
                                                    .ticket =
                                                            RateLimitTicketPayload{
                                                                    .rule_revision = ticket.rule_revision,
                                                                    .generation = ticket.generation,
                                                                    .window_start_millis = ticket.window_start_millis,
                                                            },
                                                    .tokens = tokens,
                                                    .count_usage = count_usage,
                                            },
                                            cat_context, trace_state);
    if (!response) {
        co_return std::unexpected(
                coordinator_error(RateLimitCoordinatorErrorCode::OwnerRequestFailed, response.error()));
    }
    co_return *response;
}

async::DetachedTask TokenRateLimitCoordinator::settle_remote(RateLimitNode owner, std::string user_id,
                                                             std::string model_name, TokenRateLimitTicket ticket,
                                                             std::int64_t tokens, bool count_usage,
                                                             RateLimitSettleCompletion completion,
                                                             std::unique_ptr<mem::BufPool> cat_context_pool,
                                                             std::optional<cat::MessageTraceContext> cat_context,
                                                             std::string trace_state) noexcept {
    FIBER_ASSERT(!owner.local);
    auto result = co_await settle_remote_and_wait(owner, user_id, model_name, ticket, tokens, count_usage,
                                                  cat_context ? &*cat_context : nullptr, trace_state);
    (void) cat_context_pool;
    RateLimitSettleOutcome outcome = RateLimitSettleOutcome::Applied;
    if (!result) {
        LOG(LOG_RATE_LIMIT, WARN) << "remote token rate limit settle failed owner=" << log::quoted(owner.node_id)
                                  << " error=" << static_cast<int>(result.error().code)
                                  << " io_error=" << common::io_err_name(result.error().remote.io_error);
        outcome = RateLimitSettleOutcome::Error;
    } else if (result->stale || !result->applied) {
        LOG(LOG_RATE_LIMIT, WARN) << "stale token rate limit settle ignored owner=" << log::quoted(owner.node_id);
        outcome = RateLimitSettleOutcome::Stale;
    }
    completion.notify(outcome);
    remote_settles_.done();
}

} // namespace fiber::ai_server
