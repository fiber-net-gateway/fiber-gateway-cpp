#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <async/Spawn.h>
#include <event/EventLoopGroup.h>

#include "limit/RateLimitHash.h"
#include "limit/RateLimitHttpCodec.h"
#include "limit/RateLimitShardRing.h"
#include "limit/TokenRateLimitCoordinator.h"
#include "limit/TokenRateLimitRemoteClient.h"
#include "limit/TokenRateLimitService.h"
#include "limit/TokenRateLimiter.h"

namespace {

using fiber::ai_server::CompiledModelRateLimitRule;
using fiber::ai_server::CompiledModelRoute;
using fiber::ai_server::RateLimitSettleCompletion;
using fiber::ai_server::RateLimitSettleOutcome;
using fiber::ai_server::TokenRateLimiterManager;
using fiber::ai_server::TokenRateLimitService;
using fiber::ai_server::WindowTokenRateLimiter;

using namespace std::chrono_literals;

CompiledModelRoute route(std::int64_t revision, std::string model_name, std::int64_t window_millis,
                         std::int64_t max_tokens, bool with_rule = true) {
    CompiledModelRoute route;
    route.model_name = std::move(model_name);
    if (with_rule) {
        route.rate_limit = CompiledModelRateLimitRule{
                .revision = revision,
                .window_duration_millis = window_millis,
                .max_tokens_per_window = max_tokens,
        };
    }
    return route;
}

struct SettleObservation {
    std::size_t calls = 0;
    RateLimitSettleOutcome outcome = RateLimitSettleOutcome::Error;
};

void observe_settle(void *context, RateLimitSettleOutcome outcome) noexcept {
    auto *observation = static_cast<SettleObservation *>(context);
    ++observation->calls;
    observation->outcome = outcome;
}

fiber::async::DetachedTask exercise_local_coordinator(fiber::ai_server::TokenRateLimitCoordinator *coordinator,
                                                      const CompiledModelRoute *model,
                                                      std::promise<bool> *done) noexcept {
    auto checked = co_await coordinator->check("u1", *model, 10'000);
    bool ok =
            checked && checked->result.allowed && checked->result.has_ticket && checked->owner && checked->owner->local;
    if (ok) {
        auto settled = co_await coordinator->settle_and_wait(*checked->owner, "u1", "m1", checked->result.ticket, 100,
                                                             true, 10'001);
        ok = settled && settled->applied && settled->usage_counted && settled->used_tokens == 100;
    }
    if (ok) {
        auto denied = co_await coordinator->check("u1", *model, 10'002);
        ok = denied && !denied->result.allowed && denied->result.used_tokens == 100;
    }
    co_await coordinator->shutdown();
    done->set_value(ok);
}

fiber::async::DetachedTask exercise_observed_local_settle(fiber::ai_server::TokenRateLimitCoordinator *coordinator,
                                                          const CompiledModelRoute *model,
                                                          std::promise<bool> *done) noexcept {
    auto checked = co_await coordinator->check("u1", *model, 10'000);
    bool ok = checked && checked->result.allowed && checked->result.has_ticket && checked->owner;
    if (!ok) {
        co_await coordinator->shutdown();
        done->set_value(false);
        co_return;
    }

    const fiber::ai_server::RateLimitNode owner = *checked->owner;
    SettleObservation applied;
    coordinator->settle(owner, "u1", "m1", checked->result.ticket, 80, true, 10'001,
                        RateLimitSettleCompletion{
                                .context = &applied,
                                .callback = observe_settle,
                        });
    ok = applied.calls == 1 && applied.outcome == RateLimitSettleOutcome::Applied;

    SettleObservation stale;
    fiber::ai_server::TokenRateLimitTicket stale_ticket = checked->result.ticket;
    ++stale_ticket.generation;
    coordinator->settle(owner, "u1", "m1", stale_ticket, 0, false, 10'002,
                        RateLimitSettleCompletion{
                                .context = &stale,
                                .callback = observe_settle,
                        });
    ok = ok && stale.calls == 1 && stale.outcome == RateLimitSettleOutcome::Stale;

    co_await coordinator->shutdown();
    SettleObservation stopped;
    coordinator->settle(owner, "u1", "m1", checked->result.ticket, 0, false, 10'003,
                        RateLimitSettleCompletion{
                                .context = &stopped,
                                .callback = observe_settle,
                        });
    ok = ok && stopped.calls == 1 && stopped.outcome == RateLimitSettleOutcome::Error;
    done->set_value(ok);
}

fiber::async::DetachedTask exercise_bypass_coordinator(fiber::ai_server::TokenRateLimitCoordinator *coordinator,
                                                       const CompiledModelRoute *model,
                                                       std::promise<bool> *done) noexcept {
    auto checked = co_await coordinator->check("u1", *model, 10'000);
    const bool ok = checked && !checked->result.rule_matched && checked->result.allowed &&
                    !checked->result.has_ticket && !checked->owner;
    co_await coordinator->shutdown();
    done->set_value(ok);
}

TEST(LlmRateLimitTest, ExtendsRecoveryProportionallyAfterOvershoot) {
    WindowTokenRateLimiter limiter;
    ASSERT_TRUE(limiter.init(60'000, 100));

    auto first = limiter.check(10'005);
    ASSERT_TRUE(first.allowed);
    EXPECT_EQ(first.used_tokens, 0);
    EXPECT_TRUE(limiter.settle(first.window_start_millis, 70));

    auto second = limiter.check(10'010);
    ASSERT_TRUE(second.allowed);
    EXPECT_EQ(second.used_tokens, 70);
    EXPECT_TRUE(limiter.settle(second.window_start_millis, 40));
    EXPECT_EQ(limiter.current_window_used_tokens(), 110);
    EXPECT_EQ(limiter.recover_at_millis(), 76'005);

    auto blocked = limiter.check(76'004);
    EXPECT_FALSE(blocked.allowed);
    auto recovered = limiter.check(76'005);
    EXPECT_TRUE(recovered.allowed);
    EXPECT_EQ(recovered.window_start_millis, 76'005);
}

TEST(LlmRateLimitTest, RejectsStaleWindowTicketAndZeroLimitImmediately) {
    WindowTokenRateLimiter limiter;
    ASSERT_TRUE(limiter.init(1'000, 10));
    auto first = limiter.check(100);
    ASSERT_TRUE(first.allowed);
    EXPECT_TRUE(limiter.settle(first.window_start_millis, 10));
    auto next = limiter.check(1'100);
    ASSERT_TRUE(next.allowed);
    EXPECT_FALSE(limiter.settle(first.window_start_millis, 1));

    WindowTokenRateLimiter zero;
    ASSERT_TRUE(zero.init(1'000, 0));
    auto denied = zero.check(500);
    EXPECT_FALSE(denied.allowed);
    EXPECT_FALSE(denied.has_ticket);
    EXPECT_EQ(denied.recover_at_millis, 1'500);
}

TEST(LlmRateLimitTest, ManagerChecksAndSettlesPerUserModelKey) {
    TokenRateLimiterManager manager;
    const CompiledModelRateLimitRule rule{
            .revision = 1,
            .window_duration_millis = 60'000,
            .max_tokens_per_window = 100,
    };

    auto first = manager.check("u1", "m1", rule, 10'000);
    ASSERT_TRUE(first.rule_matched);
    ASSERT_TRUE(first.allowed);
    ASSERT_TRUE(first.has_ticket);
    EXPECT_EQ(first.recover_at_millis, 70'000);

    auto settled = manager.settle("u1", "m1", first.ticket, 80, true, 10'001);
    EXPECT_TRUE(settled.applied);
    EXPECT_TRUE(settled.usage_counted);
    EXPECT_EQ(settled.used_tokens, 80);

    auto same = manager.check("u1", "m1", rule, 10'002);
    EXPECT_TRUE(same.allowed);
    EXPECT_EQ(same.used_tokens, 80);

    auto other_user = manager.check("u2", "m1", rule, 10'002);
    EXPECT_TRUE(other_user.allowed);
    EXPECT_EQ(other_user.used_tokens, 0);
}

TEST(LlmRateLimitTest, PinnedRuleRevisionKeepsOldTicketSettleable) {
    TokenRateLimiterManager manager;
    const CompiledModelRateLimitRule first_rule{
            .revision = 1,
            .window_duration_millis = 60'000,
            .max_tokens_per_window = 100,
    };
    const CompiledModelRateLimitRule second_rule{
            .revision = 2,
            .window_duration_millis = 120'000,
            .max_tokens_per_window = 200,
    };
    auto first = manager.check("u1", "m1", first_rule, 10'000);
    ASSERT_TRUE(first.has_ticket);

    auto second = manager.check("u1", "m1", second_rule, 10'001);
    ASSERT_TRUE(second.has_ticket);
    EXPECT_NE(second.ticket.rule_revision, first.ticket.rule_revision);

    auto old = manager.settle("u1", "m1", first.ticket, 30, true, 10'002);
    EXPECT_TRUE(old.applied);
    EXPECT_TRUE(old.usage_counted);

    auto current = manager.settle("u1", "m1", second.ticket, 40, true, 10'003);
    EXPECT_TRUE(current.applied);
    EXPECT_TRUE(current.usage_counted);
    EXPECT_EQ(current.used_tokens, 40);

    EXPECT_EQ(manager.check("u1", "m1", first_rule, 10'004).used_tokens, 30);
    EXPECT_EQ(manager.check("u1", "m1", second_rule, 10'004).used_tokens, 40);
}

TEST(LlmRateLimitTest, NoUsageSettlementReleasesInFlightAndSweepHonorsIdleTtl) {
    TokenRateLimiterManager manager(5'000);
    const CompiledModelRateLimitRule rule{
            .revision = 1,
            .window_duration_millis = 1'000,
            .max_tokens_per_window = 100,
    };
    auto check = manager.check("u1", "m1", rule, 10'000);
    ASSERT_TRUE(check.has_ticket);
    EXPECT_EQ(manager.stats().in_flight_count, 1u);

    EXPECT_EQ(manager.sweep_expired(20'000), 0u);
    auto settled = manager.settle("u1", "m1", check.ticket, 0, false, 20'000);
    EXPECT_TRUE(settled.applied);
    EXPECT_FALSE(settled.usage_counted);
    EXPECT_EQ(manager.stats().in_flight_count, 0u);

    EXPECT_EQ(manager.sweep_expired(24'999), 0u);
    EXPECT_EQ(manager.sweep_expired(25'000), 1u);
    EXPECT_EQ(manager.stats().limiter_count, 0u);
}

TEST(LlmRateLimitTest, DifferentRuleRevisionsKeepIndependentState) {
    TokenRateLimiterManager manager;
    const CompiledModelRateLimitRule first_rule{
            .revision = 1,
            .window_duration_millis = 60'000,
            .max_tokens_per_window = 100,
    };
    const CompiledModelRateLimitRule readded_rule{
            .revision = 3,
            .window_duration_millis = 60'000,
            .max_tokens_per_window = 100,
    };
    auto first = manager.check("u1", "m1", first_rule, 10'000);
    ASSERT_TRUE(first.has_ticket);
    EXPECT_TRUE(manager.settle("u1", "m1", first.ticket, 90, true, 10'001).usage_counted);

    auto recreated = manager.check("u1", "m1", readded_rule, 10'003);
    EXPECT_TRUE(recreated.rule_matched);
    EXPECT_TRUE(recreated.allowed);
    EXPECT_EQ(recreated.used_tokens, 0);
    EXPECT_EQ(manager.check("u1", "m1", first_rule, 10'004).used_tokens, 90);
}

TEST(LlmRateLimitTest, MurmurHashMatchesJavaGoldenValues) {
    EXPECT_EQ(fiber::ai_server::rate_limit_hash64("user-1"), 7038226039998199158ULL);
    EXPECT_EQ(fiber::ai_server::rate_limit_key_hash64("alice", "model-a"), 14943882197569545793ULL);
}

TEST(LlmRateLimitTest, SharedServiceKeepsOneKeyConsistentAcrossWorkers) {
    TokenRateLimitService service(4);
    const CompiledModelRateLimitRule rule{
            .revision = 1,
            .window_duration_millis = 60'000,
            .max_tokens_per_window = 100,
    };

    auto first = service.check("u1", "m1", rule, 10'000);
    ASSERT_TRUE(first.has_ticket);
    EXPECT_TRUE(service.settle("u1", "m1", first.ticket, 100, true, 10'001).usage_counted);

    auto denied = service.check("u1", "m1", rule, 10'002);
    EXPECT_FALSE(denied.allowed);
    EXPECT_EQ(denied.used_tokens, 100);
    EXPECT_TRUE(service.check("u2", "m1", rule, 10'002).allowed);
}

TEST(LlmRateLimitTest, CoordinatorWaitsForLocalSettlement) {
    fiber::event::EventLoopGroup workers(1);
    TokenRateLimitService service(1);
    const CompiledModelRoute model = route(1, "m1", 60'000, 100);

    fiber::ai_server::RateLimitShardRing ring;
    ASSERT_TRUE(ring.update(1, {
                                       {.node_id = "0100007f901f", .host = "127.0.0.1", .port = 8080, .local = true},
                               }));
    fiber::ai_server::TokenRateLimitRemoteClient remote(workers);
    fiber::ai_server::TokenRateLimitCoordinator coordinator(service, ring, remote);
    ASSERT_TRUE(coordinator.init());

    std::promise<bool> done;
    auto completed = done.get_future();
    workers.start();
    fiber::async::spawn(workers.at(0), [&]() { return exercise_local_coordinator(&coordinator, &model, &done); });

    ASSERT_EQ(completed.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(completed.get());
    workers.stop();
    workers.join();
}

TEST(LlmRateLimitTest, CoordinatorBypassesPinnedRouteWithoutRuleBeforeRingLookup) {
    fiber::event::EventLoopGroup workers(1);
    TokenRateLimitService service(1);
    const CompiledModelRoute model = route(1, "m1", 60'000, 100, false);

    fiber::ai_server::RateLimitShardRing ring;
    fiber::ai_server::TokenRateLimitRemoteClient remote(workers);
    fiber::ai_server::TokenRateLimitCoordinator coordinator(service, ring, remote);
    ASSERT_TRUE(coordinator.init());

    std::promise<bool> done;
    auto completed = done.get_future();
    workers.start();
    fiber::async::spawn(workers.at(0), [&]() { return exercise_bypass_coordinator(&coordinator, &model, &done); });

    ASSERT_EQ(completed.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(completed.get());
    workers.stop();
    workers.join();
}

TEST(LlmRateLimitTest, CoordinatorReportsBestEffortSettlementOutcomeExactlyOnce) {
    fiber::event::EventLoopGroup workers(1);
    TokenRateLimitService service(1);
    const CompiledModelRoute model = route(1, "m1", 60'000, 100);

    fiber::ai_server::RateLimitShardRing ring;
    ASSERT_TRUE(ring.update(1, {
                                       {.node_id = "0100007f901f", .host = "127.0.0.1", .port = 8080, .local = true},
                               }));
    fiber::ai_server::TokenRateLimitRemoteClient remote(workers);
    fiber::ai_server::TokenRateLimitCoordinator coordinator(service, ring, remote);
    ASSERT_TRUE(coordinator.init());

    std::promise<bool> done;
    auto completed = done.get_future();
    workers.start();
    fiber::async::spawn(workers.at(0), [&]() { return exercise_observed_local_settle(&coordinator, &model, &done); });

    ASSERT_EQ(completed.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(completed.get());
    workers.stop();
    workers.join();
}

TEST(LlmRateLimitTest, BuildsJavaCompatibleSelfServiceNodeId) {
    auto node_id = fiber::ai_server::java_self_service_node_id("127.0.0.1", 8080);

    ASSERT_TRUE(node_id);
    EXPECT_EQ(*node_id, "0100007f901f");
    EXPECT_FALSE(fiber::ai_server::java_self_service_node_id("::1", 8080));
    EXPECT_FALSE(fiber::ai_server::java_self_service_node_id("0.0.0.0", 8080));
}

TEST(LlmRateLimitTest, ConsistentHashRingIsStableAndUsesOneOwner) {
    fiber::ai_server::RateLimitShardRing ring;
    ASSERT_TRUE(ring.update(7, {
                                       {.node_id = "0100007f901f", .host = "127.0.0.1", .port = 8080, .local = true},
                                       {.node_id = "0200007f901f", .host = "127.0.0.2", .port = 8080},
                               }));

    const auto snapshot = ring.snapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->version, 7u);
    EXPECT_EQ(snapshot->nodes.size(), 2u);
    EXPECT_EQ(snapshot->entries.size(), 400u);

    const auto first = ring.locate("alice", "model-a");
    const auto second = ring.locate("alice", "model-a");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(*first, *second);
}

TEST(LlmRateLimitTest, ConsistentHashRingRejectsDuplicateNodes) {
    fiber::ai_server::RateLimitShardRing ring;
    auto result = ring.update(1, {
                                         {.node_id = "node", .host = "127.0.0.1", .port = 8080},
                                         {.node_id = "node", .host = "127.0.0.2", .port = 8080},
                                 });

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, fiber::ai_server::RateLimitRingErrorCode::DuplicateNode);
}

TEST(LlmRateLimitTest, InternalHttpPayloadsRoundTrip) {
    fiber::ai_server::RateLimitCheckRequest check_request{
            .user_id = "alice",
            .model_name = "model-a",
            .rule_revision = -17,
            .window_duration_millis = 60'000,
            .max_tokens_per_window = 100,
    };
    auto encoded_check_request = fiber::ai_server::encode_rate_limit_check_request(check_request);
    ASSERT_TRUE(encoded_check_request);
    fiber::mem::BufPool check_request_pool;
    auto decoded_check_request =
            fiber::ai_server::decode_rate_limit_check_request(*encoded_check_request, check_request_pool);
    ASSERT_TRUE(decoded_check_request);
    EXPECT_EQ(decoded_check_request->rule_revision, -17);
    EXPECT_EQ(decoded_check_request->window_duration_millis, 60'000);
    EXPECT_EQ(decoded_check_request->max_tokens_per_window, 100);

    fiber::ai_server::RateLimitCheckResponse check{
            .rule_matched = true,
            .allowed = true,
            .used_tokens = 7,
            .max_tokens = 100,
            .recover_at_millis = 12345,
            .ticket =
                    fiber::ai_server::RateLimitTicketPayload{
                            .rule_revision = -17,
                            .generation = 9,
                            .window_start_millis = 12000,
                    },
    };
    auto encoded_check = fiber::ai_server::encode_rate_limit_check_response(check);
    ASSERT_TRUE(encoded_check);
    fiber::mem::BufPool check_pool;
    auto decoded_check = fiber::ai_server::decode_rate_limit_check_response(*encoded_check, check_pool);
    ASSERT_TRUE(decoded_check);
    EXPECT_TRUE(decoded_check->rule_matched);
    EXPECT_TRUE(decoded_check->allowed);
    EXPECT_EQ(decoded_check->used_tokens, 7);
    ASSERT_TRUE(decoded_check->ticket);
    EXPECT_EQ(decoded_check->ticket->rule_revision, -17);
    EXPECT_EQ(decoded_check->ticket->generation, 9u);

    fiber::ai_server::RateLimitSettleRequest settle{
            .user_id = "alice",
            .model_name = "model-a",
            .ticket =
                    fiber::ai_server::RateLimitTicketPayload{
                            .rule_revision = -17,
                            .generation = 9,
                            .window_start_millis = 12000,
                    },
            .tokens = 33,
            .count_usage = true,
    };
    auto encoded_settle = fiber::ai_server::encode_rate_limit_settle_request(settle);
    ASSERT_TRUE(encoded_settle);
    fiber::mem::BufPool settle_pool;
    auto decoded_settle = fiber::ai_server::decode_rate_limit_settle_request(*encoded_settle, settle_pool);
    ASSERT_TRUE(decoded_settle);
    EXPECT_EQ(decoded_settle->user_id, "alice");
    EXPECT_EQ(decoded_settle->model_name, "model-a");
    ASSERT_TRUE(decoded_settle->ticket);
    EXPECT_EQ(decoded_settle->ticket->rule_revision, -17);
    EXPECT_EQ(decoded_settle->tokens, 33);
    EXPECT_TRUE(decoded_settle->count_usage);
}

TEST(LlmRateLimitTest, InternalHttpPayloadsRejectInvalidPinnedRule) {
    fiber::mem::BufPool pool;
    auto decoded = fiber::ai_server::decode_rate_limit_check_request(
            R"({"userId":"alice","modelName":"m","ruleRevision":1,"windowDurationMillis":0,"maxTokensPerWindow":10})",
            pool);

    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code, fiber::ai_server::RateLimitPayloadErrorCode::InvalidValue);
}

TEST(LlmRateLimitTest, InternalHttpPayloadsRejectInvalidSettlement) {
    fiber::mem::BufPool pool;
    auto decoded = fiber::ai_server::decode_rate_limit_settle_request(
            R"({"userId":"alice","modelName":"m","ticket":null,"tokens":1,"countUsage":false})", pool);

    ASSERT_FALSE(decoded);
    EXPECT_EQ(decoded.error().code, fiber::ai_server::RateLimitPayloadErrorCode::InvalidValue);
}

} // namespace
