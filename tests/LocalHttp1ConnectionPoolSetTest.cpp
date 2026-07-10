#include <gtest/gtest.h>

#include <future>

#include "async/Spawn.h"
#include "event/EventLoopGroup.h"
#include "http/Http1ConnectionGroupKey.h"
#include "http/LocalHttp1ConnectionPoolSet.h"

TEST(LocalHttp1ConnectionPoolSetTest, CreatesOnePoolPerLoopAndInitializesThem) {
    fiber::event::EventLoopGroup group(3);
    fiber::http::LocalHttp1ConnectionPoolSet::Options options{};
    options.max_idle_per_group = 4;
    options.max_idle_total = 9;
    options.initial_group_capacity = 7;

    fiber::http::LocalHttp1ConnectionPoolSet set(group, options);

    ASSERT_EQ(set.size(), 3U);
    EXPECT_TRUE(set.init());
    EXPECT_EQ(set.options().max_idle_per_group, options.max_idle_per_group);
    EXPECT_EQ(set.options().max_idle_total, options.max_idle_total);
    EXPECT_EQ(set.options().initial_group_capacity, options.initial_group_capacity);
}

TEST(LocalHttp1ConnectionPoolSetTest, AcquireUsesCurrentEventLoopShard) {
    fiber::event::EventLoopGroup group(2);
    fiber::http::LocalHttp1ConnectionPoolSet set(group);
    ASSERT_TRUE(set.init());

    std::promise<bool> loop0_promise;
    std::promise<bool> loop1_promise;
    auto loop0_future = loop0_promise.get_future();
    auto loop1_future = loop1_promise.get_future();

    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), 80,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        auto lease = set.acquire(key);
        auto conn_result = lease.emplace_connection({});
        bool ok = lease.valid() && !lease.hit() && conn_result.has_value() && lease.get() != nullptr &&
                  &lease.connection().loop() == &fiber::event::EventLoop::current() &&
                  &set.loop() == &fiber::event::EventLoop::current() && set.idle_total() == 0 && set.group_count() == 0;
        lease.reset();
        loop0_promise.set_value(ok);
        co_return;
    });
    fiber::async::spawn(group.at(1), [&]() -> fiber::async::DetachedTask {
        auto lease = set.acquire(key);
        auto conn_result = lease.emplace_connection({});
        bool ok = lease.valid() && !lease.hit() && conn_result.has_value() && lease.get() != nullptr &&
                  &lease.connection().loop() == &fiber::event::EventLoop::current() &&
                  &set.loop() == &fiber::event::EventLoop::current() && set.idle_total() == 0 && set.group_count() == 0;
        lease.reset();
        loop1_promise.set_value(ok);
        co_return;
    });

    EXPECT_TRUE(loop0_future.get());
    EXPECT_TRUE(loop1_future.get());
    group.stop();
    group.join();
}
