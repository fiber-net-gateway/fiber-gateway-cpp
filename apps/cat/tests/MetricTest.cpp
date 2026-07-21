#include <gtest/gtest.h>

#include <chrono>
#include <type_traits>
#include <utility>

#include <event/EventLoop.h>
#include <fiber/cat/Metric.h>

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

} // namespace
