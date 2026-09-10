#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <optional>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/quic/QuicConnection.h>

#include "QuicTestLoop.h"

namespace {

using fiber::common::IoErr;
using fiber::quic::QuicConnection;

QuicConnection::Options client_options(fiber::event::EventLoop &loop) noexcept {
    QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Client;
    options.loop = &loop;
    return options;
}

IoErr to_err(const fiber::common::IoResult<void> &result) noexcept { return result ? IoErr::None : result.error(); }

fiber::async::DetachedTask wait_established_into(QuicConnection *conn, std::chrono::milliseconds timeout,
                                                 std::promise<IoErr> *done) {
    done->set_value(to_err(co_await conn->wait_established(timeout)));
}

fiber::async::DetachedTask wait_confirmed_into(QuicConnection *conn, std::chrono::milliseconds timeout,
                                               std::promise<IoErr> *done) {
    done->set_value(to_err(co_await conn->wait_confirmed(timeout)));
}

} // namespace

// The reason waiters are re-evaluated one by one instead of being handed a
// single result: reaching Established answers a plain waiter and leaves a
// waiter for confirmation exactly where it was.
TEST(QuicHandshakeGateTest, EstablishedWakesOnlyThePlainWaiter) {
    fiber::event::EventLoopGroup group(1);
    QuicConnection conn(client_options(group.at(0)));

    std::promise<IoErr> established_done;
    std::promise<IoErr> confirmed_done;
    auto established = established_done.get_future();
    auto confirmed = confirmed_done.get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [&]() { return wait_established_into(&conn, std::chrono::seconds(5), &established_done); });
    fiber::async::spawn(group.at(0),
                        [&]() { return wait_confirmed_into(&conn, std::chrono::seconds(5), &confirmed_done); });
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        co_await fiber::async::sleep(std::chrono::milliseconds(20));
        EXPECT_TRUE(conn.mark_established().has_value());
        co_await fiber::async::sleep(std::chrono::milliseconds(200));
        conn.confirm_handshake();
    });

    ASSERT_EQ(established.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(established.get(), IoErr::None);

    // Established resolved; confirmation has not happened yet, so the other
    // waiter must still be parked rather than woken alongside it.
    EXPECT_EQ(confirmed.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);

    ASSERT_EQ(confirmed.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(confirmed.get(), IoErr::None);

    group.stop();
    group.join();
}

// A close resolves waiters with why it closed, not with a blanket Canceled.
TEST(QuicHandshakeGateTest, PeerCloseResolvesWaitersWithConnReset) {
    fiber::event::EventLoopGroup group(1);
    QuicConnection conn(client_options(group.at(0)));

    std::promise<IoErr> done;
    auto future = done.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return wait_established_into(&conn, std::chrono::seconds(5), &done); });
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        co_await fiber::async::sleep(std::chrono::milliseconds(20));
        conn.begin_draining(fiber::quic::QuicErrorCode::NoError);
    });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(future.get(), IoErr::ConnReset);

    group.stop();
    group.join();
}

TEST(QuicHandshakeGateTest, WaitTimesOutWhileHandshakeIsInFlight) {
    fiber::event::EventLoopGroup group(1);
    QuicConnection conn(client_options(group.at(0)));

    std::promise<IoErr> done;
    auto future = done.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(conn.start_handshake().has_value());
        co_return;
    });
    fiber::async::spawn(group.at(0),
                        [&]() { return wait_established_into(&conn, std::chrono::milliseconds(20), &done); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(future.get(), IoErr::TimedOut);
    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Handshaking);

    group.stop();
    group.join();
}

// Already-resolved waits never touch the queue at all.
TEST(QuicHandshakeGateTest, WaitOnAnEstablishedConnectionReturnsImmediately) {
    fiber::event::EventLoopGroup group(1);
    QuicConnection conn(client_options(group.at(0)));

    std::promise<IoErr> established_done;
    std::promise<IoErr> confirmed_done;
    auto established = established_done.get_future();
    auto confirmed = confirmed_done.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(conn.mark_established().has_value());
        // A poll timeout would fail if this had to suspend.
        established_done.set_value(to_err(co_await conn.wait_established(std::chrono::milliseconds::zero())));
        // Confirmation has not happened, so this one does time out.
        confirmed_done.set_value(to_err(co_await conn.wait_confirmed(std::chrono::milliseconds::zero())));
    });

    ASSERT_EQ(established.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(established.get(), IoErr::None);
    ASSERT_EQ(confirmed.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(confirmed.get(), IoErr::TimedOut);

    group.stop();
    group.join();
}
