#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/cat/Cat.h>
#include <fiber/cat/Metric.h>
#include <fiber/event/EventLoop.h>
#include <fiber/net/IpAddress.h>

namespace {

using namespace std::chrono_literals;
using fiber::cat::Metric;
using fiber::cat::MetricKind;
using fiber::cat::RecordError;

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

TEST(CatMetricTest, RecordsCountAndResetsSnapshot) {
    run_on_loop([] {
        auto metric_result = Metric::create_count("requests");
        ASSERT_TRUE(metric_result);
        Metric metric = std::move(*metric_result);
        EXPECT_EQ(metric.kind(), MetricKind::Count);
        EXPECT_EQ(metric.name(), "requests");
        EXPECT_EQ(metric.record_count(), RecordError::None);
        EXPECT_EQ(metric.record_count(4), RecordError::None);

        auto snapshot = metric.snapshot_and_reset();
        ASSERT_TRUE(snapshot);
        EXPECT_EQ(snapshot->name, "requests");
        EXPECT_EQ(snapshot->kind, MetricKind::Count);
        EXPECT_EQ(snapshot->quantity, 5);
        EXPECT_EQ(snapshot->duration_sum_millis, 0U);

        auto empty = metric.snapshot();
        ASSERT_TRUE(empty);
        EXPECT_EQ(empty->quantity, 0);
        EXPECT_EQ(metric.record_duration(1ms), RecordError::WrongMetricKind);
    });
}

TEST(CatMetricTest, RecordsDurationCountAndSum) {
    run_on_loop([] {
        auto metric_result = Metric::create_duration("latency");
        ASSERT_TRUE(metric_result);
        Metric metric = std::move(*metric_result);
        EXPECT_EQ(metric.record_duration(12ms), RecordError::None);
        EXPECT_EQ(metric.record_duration(8ms), RecordError::None);
        EXPECT_EQ(metric.record_duration(-1ms), RecordError::LimitExceeded);
        EXPECT_EQ(metric.record_count(), RecordError::WrongMetricKind);

        auto snapshot = metric.snapshot();
        ASSERT_TRUE(snapshot);
        EXPECT_EQ(snapshot->kind, MetricKind::Duration);
        EXPECT_EQ(snapshot->quantity, 2);
        EXPECT_EQ(snapshot->duration_sum_millis, 20U);
        EXPECT_EQ(metric.reset(), RecordError::None);
        snapshot = metric.snapshot();
        ASSERT_TRUE(snapshot);
        EXPECT_EQ(snapshot->quantity, 0);
        EXPECT_EQ(snapshot->duration_sum_millis, 0U);
    });
}

TEST(CatMetricTest, RequiresOwnerEventLoop) {
    auto metric = Metric::create_count("outside-loop");
    ASSERT_FALSE(metric);
    EXPECT_EQ(metric.error(), RecordError::WrongEventLoop);
}

fiber::async::DetachedTask run_bound_metric_case(fiber::event::EventLoop *loop,
                                                 std::promise<fiber::cat::CatClientStats> *promise) {
    fiber::cat::CatClientConfigParams params{
            .app_key = "metrics",
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
    options.aggregation_flush_interval = 5ms;
    options.collector_connect_timeout = 1ms;
    options.shutdown_drain_timeout = 0ms;
    auto created = fiber::cat::CatClient::create(*loop, std::move(*config), options);
    if (!created || !(*created)->start()) {
        promise->set_value({});
        loop->stop();
        co_return;
    }
    std::unique_ptr<fiber::cat::CatClient> client = std::move(*created);
    auto count = Metric::create_count(*client, "requests");
    auto duration = Metric::create_duration(*client, "latency");
    if (count && duration) {
        EXPECT_TRUE(count->automatically_reported());
        EXPECT_EQ(count->record_count(5), RecordError::None);
        EXPECT_EQ(count->record_count(-2), RecordError::None);
        EXPECT_EQ(count->record_count(0), RecordError::None);
        EXPECT_EQ(duration->record_duration(0ms), RecordError::None);
        EXPECT_EQ(duration->record_duration(8ms), RecordError::None);
        auto snapshot = count->snapshot();
        EXPECT_FALSE(snapshot);
        EXPECT_EQ(snapshot.error(), RecordError::InvalidArgument);
    }
    const auto deadline = loop->now() + 200ms;
    while (client->stats().metric_submitted == 0 && loop->now() < deadline) {
        co_await fiber::async::sleep(1ms);
    }
    const auto stats = client->stats();
    co_await client->shutdown();
    promise->set_value(stats);
    loop->stop();
}

TEST(CatMetricTest, ClientBoundMetricsFlushAsOneSystemAggregate) {
    fiber::event::EventLoop loop;
    std::promise<fiber::cat::CatClientStats> promise;
    auto future = promise.get_future();
    fiber::async::spawn(loop, [&] { return run_bound_metric_case(&loop, &promise); });
    loop.run();
    const auto stats = future.get();
    EXPECT_EQ(stats.metric_observations, 5);
    EXPECT_EQ(stats.metric_overflow, 0);
    EXPECT_EQ(stats.metric_submitted, 1);
    EXPECT_EQ(stats.submitted_messages, 1);
}

fiber::async::DetachedTask run_detach_shard_case(fiber::event::EventLoop *loop,
                                                 std::promise<fiber::cat::CatClientStats> *promise) {
    fiber::cat::CatClientConfigParams params{
            .app_key = "detach",
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
    options.aggregation_flush_interval = 1h;
    options.collector_connect_timeout = 1ms;
    options.shutdown_drain_timeout = 0ms;
    auto created = fiber::cat::CatClient::create(*loop, std::move(*config), options);
    if (!created || !(*created)->start()) {
        promise->set_value({});
        loop->stop();
        co_return;
    }
    std::unique_ptr<fiber::cat::CatClient> client = std::move(*created);
    {
        auto metric = Metric::create_count(*client, "requests");
        if (metric) {
            EXPECT_EQ(metric->record_count(3), RecordError::None);
        }
    }
    EXPECT_EQ(co_await client->detach_current_event_loop(), RecordError::None);
    const auto stats = client->stats();
    co_await client->shutdown();
    promise->set_value(stats);
    loop->stop();
}

TEST(CatMetricTest, ExplicitLoopDetachFlushesAndQuiescesAggregationShard) {
    fiber::event::EventLoop loop;
    std::promise<fiber::cat::CatClientStats> promise;
    auto future = promise.get_future();
    fiber::async::spawn(loop, [&] { return run_detach_shard_case(&loop, &promise); });
    loop.run();
    const auto stats = future.get();
    EXPECT_EQ(stats.metric_observations, 1);
    EXPECT_EQ(stats.metric_submitted, 1);
    EXPECT_EQ(stats.submitted_messages, 1);
}

fiber::async::DetachedTask run_repeated_detach_case(fiber::event::EventLoop *loop,
                                                    std::promise<fiber::cat::CatClientStats> *promise) {
    fiber::cat::CatClientConfigParams params{
            .app_key = "repeated-detach",
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
    options.aggregation_flush_interval = 1h;
    options.collector_connect_timeout = 1ms;
    options.shutdown_drain_timeout = 0ms;
    auto created = fiber::cat::CatClient::create(*loop, std::move(*config), options);
    if (!created || !(*created)->start()) {
        promise->set_value({});
        loop->stop();
        co_return;
    }
    std::unique_ptr<fiber::cat::CatClient> client = std::move(*created);
    constexpr std::size_t cycles = 80;
    bool success = true;
    for (std::size_t index = 0; index < cycles; ++index) {
        {
            auto metric = Metric::create_count(*client, "requests");
            if (!metric || metric->record_count(1) != RecordError::None) {
                success = false;
                break;
            }
        }
        if (co_await client->detach_current_event_loop() != RecordError::None) {
            success = false;
            break;
        }
    }
    const auto stats = success ? client->stats() : fiber::cat::CatClientStats{};
    co_await client->shutdown();
    promise->set_value(stats);
    loop->stop();
}

TEST(CatMetricTest, RepeatedRegistrationAndDetachReclaimsRetiredShards) {
    fiber::event::EventLoop loop;
    std::promise<fiber::cat::CatClientStats> promise;
    auto future = promise.get_future();
    fiber::async::spawn(loop, [&] { return run_repeated_detach_case(&loop, &promise); });
    loop.run();
    const auto stats = future.get();
    EXPECT_EQ(stats.metric_observations, 80);
    EXPECT_EQ(stats.metric_submitted, 80);
    EXPECT_EQ(stats.submitted_messages, 80);
}

} // namespace
