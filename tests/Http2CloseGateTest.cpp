#include <gtest/gtest.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Http2CloseGate.h>
#include <future>
#include <memory>
#include <vector>

namespace {
using namespace fiber;
using namespace std::chrono_literals;

struct Owner {
    Owner() :
        connection(options(), this, {nullptr, &changed, nullptr}), gate(event::EventLoop::current(), connection) {}
    static http::Http2Connection::Options options() {
        http::Http2Connection::Options opts;
        opts.role = http::Http2Connection::ConnectionRole::Client;
        return opts;
    }
    static void changed(void *ctx, http::Http2Connection &conn) noexcept {
        auto &self = *static_cast<Owner *>(ctx);
        if (conn.state() == http::Http2Connection::State::Closed) {
            self.gate.on_connection_closed();
            EXPECT_FALSE(self.gate.closed());
            ++self.notifications;
        }
    }
    http::Http2Connection connection;
    http::Http2CloseGate gate;
    unsigned notifications = 0;
};

template<class Fn>
void run(Fn fn) {
    event::EventLoopGroup group(1);
    std::promise<void> done;
    auto future = done.get_future();
    group.start();
    async::spawn(group.at(0), [&]() -> async::DetachedTask {
        co_await fn();
        done.set_value();
    });
    EXPECT_EQ(future.wait_for(5s), std::future_status::ready);
    group.at(0).stop();
    group.join();
}
} // namespace

TEST(Http2CloseGateTest, ClosureIsDeferredAndObserversPrecedeJoiners) {
    run([]() -> async::Task<void> {
        auto owner = std::make_unique<Owner>();
        std::vector<int> order;
        http::Http2CloseGate::ObserverHook hook;
        owner->gate.add_observer(
                hook,
                [](void *ctx, http::Http2Connection &, common::IoErr) noexcept {
                    static_cast<std::vector<int> *>(ctx)->push_back(1);
                },
                &order);
        owner->connection.shutdown();
        owner->connection.shutdown();
        EXPECT_EQ(owner->notifications, 1u);
        EXPECT_FALSE(owner->gate.closed());
        EXPECT_TRUE(order.empty());
        EXPECT_TRUE((co_await owner->gate.join()).has_value());
        order.push_back(2);
        EXPECT_EQ(order, (std::vector<int>{1, 2}));
        EXPECT_TRUE(owner->gate.closed());
        EXPECT_FALSE(hook.linked);
        EXPECT_TRUE((co_await owner->gate.join()).has_value());
        owner.reset();
    });
}

TEST(Http2CloseGateTest, ObserverCanRemoveAndDestroyNextObserver) {
    run([]() -> async::Task<void> {
        Owner owner;
        http::Http2CloseGate::ObserverHook first;
        auto second = std::make_unique<http::Http2CloseGate::ObserverHook>();
        owner.gate.add_observer(
                first,
                [](void *ctx, http::Http2Connection &, common::IoErr) noexcept {
                    static_cast<std::unique_ptr<http::Http2CloseGate::ObserverHook> *>(ctx)->reset();
                },
                &second);
        owner.gate.add_observer(
                *second,
                [](void *, http::Http2Connection &, common::IoErr) noexcept {
                    ADD_FAILURE() << "removed observer was invoked";
                },
                nullptr);
        owner.connection.shutdown();
        (void) co_await owner.gate.join();
        EXPECT_FALSE(second);
    });
}

TEST(Http2CloseGateTest, LateGateStillDefersCompletion) {
    run([]() -> async::Task<void> {
        http::Http2Connection conn(Owner::options(), nullptr, {});
        conn.shutdown();
        http::Http2CloseGate gate(event::EventLoop::current(), conn);
        EXPECT_FALSE(gate.closed());
        gate.on_connection_closed();
        EXPECT_TRUE((co_await gate.join()).has_value());
        EXPECT_TRUE(gate.closed());
    });
}

TEST(Http2CloseGateTest, PendingTeardownCancelsDeferredEntry) {
    run([]() -> async::Task<void> {
        {
            Owner owner;
            owner.connection.shutdown();
            EXPECT_FALSE(owner.gate.closed());
        }
        co_await async::sleep(1ms);
    });
}
