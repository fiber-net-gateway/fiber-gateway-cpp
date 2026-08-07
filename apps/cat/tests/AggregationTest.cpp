#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/cat/CatClientConfig.h>
#include <fiber/cat/Status.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/event/EventLoop.h>
#include <fiber/net/IpAddress.h>

#include "CatAggregation.h"
#include "CatClientCore.h"
#include "CatInternal.h"

namespace {

using namespace std::chrono_literals;
using fiber::cat::RecordError;
using fiber::cat::detail::AggregateKind;
using fiber::cat::detail::AggregateValue;

template<typename F>
struct LoopCall {
    F callback;
    fiber::event::EventLoop *loop = nullptr;
    fiber::event::EventLoop::NotifyEntry entry{};

    static void run(LoopCall *call) noexcept {
        call->callback();
        call->loop->stop();
    }
};

template<typename F>
void run_on_loop(F &&callback) {
    fiber::event::EventLoop loop;
    LoopCall<std::decay_t<F>> call{.callback = std::forward<F>(callback), .loop = &loop};
    loop.post<LoopCall<std::decay_t<F>>, &LoopCall<std::decay_t<F>>::entry, &LoopCall<std::decay_t<F>>::run>(call);
    loop.run();
}

void freeze_message(fiber::cat::detail::MessageData &message) {
    message.completed = true;
    if (message.kind != fiber::cat::MessageKind::Transaction) {
        return;
    }
    auto &transaction = static_cast<fiber::cat::detail::TransactionData &>(message);
    std::size_t visited = 0;
    for (auto *chunk = transaction.children_head; chunk; chunk = chunk->next) {
        const std::size_t count = std::min(fiber::cat::detail::kChildrenPerChunk, transaction.child_count - visited);
        for (std::size_t index = 0; index < count; ++index) {
            freeze_message(*chunk->children[index]);
        }
        visited += count;
    }
}

TEST(CatAggregationTest, AggregatesTransactionEventErrorsAndDurationsByKey) {
    run_on_loop([] {
        auto *shard =
                fiber::cat::detail::AggregationShard::create(fiber::event::EventLoop::current(), 8, 128, 64 * 1024, 16);
        ASSERT_NE(shard, nullptr);

        fiber::mem::BufPool pool;
        auto root_created = fiber::cat::detail::create_transaction_root(pool, "OldCall", "old-root", {});
        ASSERT_TRUE(root_created);
        auto *root = *root_created;
        auto *trace = root->trace;
        ASSERT_EQ(fiber::cat::detail::set_type(root, "Call"), RecordError::None);
        ASSERT_EQ(fiber::cat::detail::set_name(root, "same"), RecordError::None);
        ASSERT_EQ(fiber::cat::detail::set_duration(root, 1500us), RecordError::None);
        ASSERT_EQ(fiber::cat::detail::set_status(root, "ERROR"), RecordError::None);

        auto child_created = fiber::cat::detail::create_transaction(*root, "Call", "same");
        ASSERT_TRUE(child_created);
        ASSERT_EQ(fiber::cat::detail::set_duration(*child_created, 2500us), RecordError::None);
        auto event_created = fiber::cat::detail::create_event(**child_created, "OldRemote", "old-failure");
        ASSERT_TRUE(event_created);
        ASSERT_EQ(fiber::cat::detail::set_type(*event_created, "Remote"), RecordError::None);
        ASSERT_EQ(fiber::cat::detail::set_name(*event_created, "failure"), RecordError::None);
        ASSERT_EQ(fiber::cat::detail::set_status(*event_created, "ERROR"), RecordError::None);

        freeze_message(*trace->data->root);
        trace->data->open_message_count = 0;
        EXPECT_EQ(shard->aggregate(*trace->data), 0);
        fiber::cat::detail::discard_message_trace(trace);

        std::vector<AggregateValue> values;
        shard->for_each(&values, [](void *opaque, const AggregateValue &value) noexcept {
            static_cast<std::vector<AggregateValue> *>(opaque)->push_back(value);
            return true;
        });
        ASSERT_EQ(values.size(), 2);
        EXPECT_EQ(values[0].kind, AggregateKind::Transaction);
        EXPECT_EQ(values[0].type, "Call");
        EXPECT_EQ(values[0].name, "same");
        EXPECT_EQ(values[0].count, 2);
        EXPECT_EQ(values[0].error_count, 1);
        EXPECT_EQ(values[0].duration_sum_millis, 3);
        EXPECT_EQ(values[1].kind, AggregateKind::Event);
        EXPECT_EQ(values[1].type, "Remote");
        EXPECT_EQ(values[1].name, "failure");
        EXPECT_EQ(values[1].count, 1);
        EXPECT_EQ(values[1].error_count, 1);
        delete shard;
    });
}

TEST(CatAggregationTest, BoundsCardinalityAndReportsDroppedMessages) {
    run_on_loop([] {
        auto *shard = fiber::cat::detail::AggregationShard::create(fiber::event::EventLoop::current(), 1, 32, 4096, 4);
        ASSERT_NE(shard, nullptr);
        fiber::mem::BufPool pool;
        auto root_created = fiber::cat::detail::create_transaction_root(pool, "T", "root", {});
        ASSERT_TRUE(root_created);
        auto *root = *root_created;
        auto *trace = root->trace;
        ASSERT_TRUE(fiber::cat::detail::create_event(*root, "E", "child"));
        freeze_message(*trace->data->root);
        trace->data->open_message_count = 0;
        EXPECT_GE(shard->aggregate(*trace->data), 1);
        EXPECT_EQ(shard->key_count(), 1);
        fiber::cat::detail::discard_message_trace(trace);
        delete shard;
    });
}

struct SamplingOutcome {
    fiber::cat::CatClientStats stats;
};

fiber::async::DetachedTask run_sampling_case(fiber::event::EventLoop *loop, std::promise<SamplingOutcome> *promise) {
    fiber::cat::CatClientConfigParams params{
            .app_key = "sampling",
            .hostname = "host",
            .ip = "127.0.0.1",
    };
    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), 1);
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        promise->set_value({});
        loop->stop();
        co_return;
    }
    fiber::cat::CatClientOptions options;
    options.initial_sample_rate = 0.0;
    options.aggregation_flush_interval = 1h;
    options.collector_connect_timeout = 1ms;
    options.shutdown_drain_timeout = 0ms;
    auto core = std::make_shared<fiber::cat::detail::CatClientCore>(*loop, std::move(*config), options, nullptr);
    if (!core->start()) {
        promise->set_value({});
        loop->stop();
        co_return;
    }

    auto *shard = core->aggregation_shard(*loop);
    fiber::mem::BufPool normal_pool;
    auto normal = fiber::cat::detail::create_transaction_root(
            normal_pool, "URL", "/sampled", {},
            {.core = core, .aggregation_shard = shard, .propagation_context = {.message_id = "normal"}});
    if (normal) {
        (void) fiber::cat::detail::set_duration(*normal, 2ms);
        (void) fiber::cat::detail::complete(*normal);
    }
    fiber::mem::BufPool problem_pool;
    auto problem = fiber::cat::detail::create_event_root(
            problem_pool, "Error", "failure", {},
            {.core = core, .aggregation_shard = shard, .propagation_context = {.message_id = "problem"}});
    if (problem) {
        (void) fiber::cat::detail::set_status(*problem, "ERROR");
        (void) fiber::cat::detail::complete(*problem);
    }
    fiber::mem::BufPool incomplete_pool;
    auto incomplete = fiber::cat::detail::create_transaction_root(
            incomplete_pool, "URL", "/incomplete", {},
            {.core = core, .aggregation_shard = shard, .propagation_context = {.message_id = "incomplete"}});
    if (incomplete) {
        fiber::cat::detail::abandon(*incomplete);
    }

    SamplingOutcome outcome;
    outcome.stats = core->stats();
    co_await core->shutdown();
    promise->set_value(outcome);
    loop->stop();
}

TEST(CatAggregationTest, SamplingZeroAggregatesNormalAndForcesProblemTrees) {
    fiber::event::EventLoop loop;
    std::promise<SamplingOutcome> promise;
    auto future = promise.get_future();
    fiber::async::spawn(loop, [&] { return run_sampling_case(&loop, &promise); });
    loop.run();
    const SamplingOutcome outcome = future.get();
    EXPECT_EQ(outcome.stats.sampled_trees, 1);
    EXPECT_EQ(outcome.stats.forced_problem_trees, 2);
    EXPECT_EQ(outcome.stats.aggregated_trees, 1);
    EXPECT_EQ(outcome.stats.submitted_messages, 2);
    EXPECT_EQ(outcome.stats.dropped_sampled, 0);
}

fiber::async::DetachedTask run_mixed_sampling_case(fiber::event::EventLoop *loop,
                                                   std::promise<SamplingOutcome> *promise) {
    fiber::cat::CatClientConfigParams params{
            .app_key = "mixed-sampling",
            .hostname = "host",
            .ip = "127.0.0.1",
    };
    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), 1);
    auto config = fiber::cat::CatClientConfig::create(std::move(params));
    if (!config) {
        promise->set_value({});
        loop->stop();
        co_return;
    }
    fiber::cat::CatClientOptions options;
    options.enable_heartbeat = false;
    options.initial_sample_rate = 0.5;
    options.aggregation_flush_interval = 1h;
    options.collector_connect_timeout = 1ms;
    options.shutdown_drain_timeout = 0ms;
    auto core = std::make_shared<fiber::cat::detail::CatClientCore>(*loop, std::move(*config), options, nullptr);
    if (!core->start()) {
        promise->set_value({});
        loop->stop();
        co_return;
    }
    auto *shard = core->aggregation_shard(*loop);
    constexpr std::size_t trees = 128;
    for (std::size_t index = 0; index < trees; ++index) {
        fiber::mem::BufPool pool;
        auto root = fiber::cat::detail::create_transaction_root(
                pool, "URL", "/mixed", {},
                {.core = core, .aggregation_shard = shard, .propagation_context = {.message_id = "mixed"}});
        if (root) {
            (void) fiber::cat::detail::set_duration(*root, 2ms);
            (void) fiber::cat::detail::complete(*root);
        }
    }
    SamplingOutcome outcome{.stats = core->stats()};
    co_await core->shutdown();
    promise->set_value(outcome);
    loop->stop();
}

TEST(CatAggregationTest, IntermediateSamplingPartitionsEachNormalTreeExactlyOnce) {
    fiber::event::EventLoop loop;
    std::promise<SamplingOutcome> promise;
    auto future = promise.get_future();
    fiber::async::spawn(loop, [&] { return run_mixed_sampling_case(&loop, &promise); });
    loop.run();
    const auto stats = future.get().stats;
    EXPECT_GT(stats.submitted_messages, 0);
    EXPECT_GT(stats.aggregated_trees, 0);
    EXPECT_EQ(stats.submitted_messages + stats.aggregated_trees, 128);
    EXPECT_EQ(stats.sampled_trees, stats.aggregated_trees);
}

} // namespace
