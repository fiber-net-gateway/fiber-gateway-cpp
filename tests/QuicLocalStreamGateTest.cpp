#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <new>
#include <thread>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/quic/QuicConnection.h>
#include <fiber/quic/QuicLocalStreamGate.h>

#include "QuicTestLoop.h"

namespace {

using fiber::common::IoErr;
using fiber::quic::QuicConnection;
using fiber::quic::QuicLocalStreamGate;
using fiber::quic::QuicStreamType;

void destroy_test_stream(void *, fiber::quic::QuicStream &stream) noexcept { delete &stream; }

fiber::quic::QuicStream::Lease make_test_stream() noexcept {
    return fiber::quic::QuicStream::Lease::adopt(new (std::nothrow)
                                                         fiber::quic::QuicStream(nullptr, destroy_test_stream));
}

// The gate is an observer like any other owner: it only learns about the
// connection through the Ops hooks, so the tests wire them the way
// Http3Connection does.
void forward_state_change(void *owner, QuicConnection &) noexcept {
    static_cast<QuicLocalStreamGate *>(owner)->on_state_change();
}

void forward_capacity_change(void *owner, QuicConnection &) noexcept {
    static_cast<QuicLocalStreamGate *>(owner)->on_capacity_change();
}

void wire_gate(QuicConnection &conn, QuicLocalStreamGate &gate) noexcept {
    QuicConnection::Ops ops{};
    ops.on_state_change = &forward_state_change;
    ops.on_capacity_change = &forward_capacity_change;
    (void) conn.set_app_ops(&gate, ops);
}

QuicConnection::Options client_options(fiber::event::EventLoop *loop) noexcept {
    QuicConnection::Options options = fiber::test::quic_options();
    options.role = fiber::quic::QuicConnectionRole::Client;
    if (loop != nullptr) {
        options.loop = loop;
    }
    return options;
}

struct AttachResult {
    bool ok = false;
    std::uint64_t stream_id = 0;
    IoErr error = IoErr::None;
};

AttachResult to_attach_result(fiber::common::IoResult<fiber::quic::QuicStream *> result) {
    if (result) {
        return {.ok = true, .stream_id = (*result)->stream_id()};
    }
    return {.ok = false, .error = result.error()};
}

fiber::async::DetachedTask attach_through_gate(QuicLocalStreamGate *gate, fiber::quic::QuicStream::Lease stream,
                                               QuicStreamType type, std::chrono::milliseconds timeout,
                                               std::promise<AttachResult> *done) {
    auto result = co_await gate->attach(std::move(stream), type, timeout);
    done->set_value(to_attach_result(result));
}

fiber::async::DetachedTask grant_max_streams_after_delay(QuicConnection *conn, QuicStreamType type, std::uint64_t limit,
                                                         std::atomic<bool> *started) {
    co_await fiber::async::sleep(std::chrono::milliseconds(20));
    started->store(true, std::memory_order_relaxed);
    fiber::quic::QuicMaxStreamsFrame frame{};
    frame.bidirectional = type == QuicStreamType::Bidirectional;
    frame.limit = limit;
    (void) conn->recv_max_streams_frame(frame);
}

fiber::async::DetachedTask shutdown_after_delay(QuicConnection *conn, std::atomic<bool> *done) {
    co_await fiber::async::sleep(std::chrono::milliseconds(20));
    conn->shutdown();
    done->store(true, std::memory_order_relaxed);
}

fiber::async::DetachedTask cancel_bidi_after_delay(QuicLocalStreamGate *gate, std::atomic<bool> *done) {
    co_await fiber::async::sleep(std::chrono::milliseconds(20));
    gate->cancel_all(QuicStreamType::Bidirectional, IoErr::Canceled);
    done->store(true, std::memory_order_relaxed);
}

// The gate is loop-affine: cancelling parks and timers has to happen on the
// connection's loop, never from the test thread.
fiber::async::DetachedTask cancel_all_on_loop(QuicLocalStreamGate *gate) noexcept {
    gate->cancel_all(IoErr::Canceled);
    co_return;
}

} // namespace

TEST(QuicLocalStreamGateTest, AttachResumesAfterMaxStreams) {
    fiber::event::EventLoopGroup group(1);
    QuicConnection::Options options = client_options(&group.at(0));
    options.max_local_bidirectional_streams = 0;
    QuicConnection conn(options);
    QuicLocalStreamGate gate(conn);
    wire_gate(conn, gate);
    ASSERT_TRUE(conn.mark_established());

    std::promise<AttachResult> done;
    auto future = done.get_future();
    std::atomic<bool> grant_seen{false};
    auto stream = make_test_stream();
    ASSERT_TRUE(stream);

    group.start();
    fiber::async::spawn(group.at(0), [&gate, stream = std::move(stream), &done]() mutable {
        return attach_through_gate(&gate, std::move(stream), QuicStreamType::Bidirectional, std::chrono::seconds(1),
                                   &done);
    });
    fiber::async::spawn(group.at(0), [&conn, &grant_seen]() {
        return grant_max_streams_after_delay(&conn, QuicStreamType::Bidirectional, 1, &grant_seen);
    });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const AttachResult result = future.get();
    EXPECT_TRUE(grant_seen.load(std::memory_order_relaxed));
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.stream_id, 0U);
    EXPECT_NE(conn.find_stream(0), nullptr);
    EXPECT_EQ(conn.active_stream_count(), 1U);
    EXPECT_FALSE(gate.has_waiters());

    group.stop();
    group.join();
}

// Closing the connection reaches the gate only through on_state_change; the
// connection no longer keeps a wait queue that it could cancel itself.
TEST(QuicLocalStreamGateTest, ConnectionShutdownCancelsWaiters) {
    fiber::event::EventLoopGroup group(1);
    QuicConnection::Options options = client_options(&group.at(0));
    options.max_local_bidirectional_streams = 0;
    QuicConnection conn(options);
    QuicLocalStreamGate gate(conn);
    wire_gate(conn, gate);
    ASSERT_TRUE(conn.mark_established());

    std::promise<AttachResult> done;
    auto future = done.get_future();
    std::atomic<bool> shut{false};
    auto stream = make_test_stream();
    ASSERT_TRUE(stream);

    group.start();
    fiber::async::spawn(group.at(0), [&gate, stream = std::move(stream), &done]() mutable {
        return attach_through_gate(&gate, std::move(stream), QuicStreamType::Bidirectional, std::chrono::seconds(5),
                                   &done);
    });
    fiber::async::spawn(group.at(0), [&conn, &shut]() { return shutdown_after_delay(&conn, &shut); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const AttachResult result = future.get();
    EXPECT_TRUE(shut.load(std::memory_order_relaxed));
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, IoErr::Canceled);
    EXPECT_FALSE(gate.has_waiters());

    group.stop();
    group.join();
}

// An owner cancels one direction when it knows something QUIC does not -- an
// HTTP/3 GOAWAY refuses new requests while the control stream is unaffected.
TEST(QuicLocalStreamGateTest, CancelAllIsPerStreamType) {
    fiber::event::EventLoopGroup group(1);
    QuicConnection::Options options = client_options(&group.at(0));
    options.max_local_bidirectional_streams = 0;
    options.max_local_unidirectional_streams = 0;
    QuicConnection conn(options);
    QuicLocalStreamGate gate(conn);
    wire_gate(conn, gate);
    ASSERT_TRUE(conn.mark_established());

    std::promise<AttachResult> bidi_done;
    std::promise<AttachResult> uni_done;
    auto bidi_future = bidi_done.get_future();
    auto uni_future = uni_done.get_future();
    std::atomic<bool> cancelled{false};
    auto bidi_stream = make_test_stream();
    auto uni_stream = make_test_stream();
    ASSERT_TRUE(bidi_stream);
    ASSERT_TRUE(uni_stream);

    group.start();
    fiber::async::spawn(group.at(0), [&gate, stream = std::move(bidi_stream), &bidi_done]() mutable {
        return attach_through_gate(&gate, std::move(stream), QuicStreamType::Bidirectional, std::chrono::seconds(5),
                                   &bidi_done);
    });
    fiber::async::spawn(group.at(0), [&gate, stream = std::move(uni_stream), &uni_done]() mutable {
        return attach_through_gate(&gate, std::move(stream), QuicStreamType::Unidirectional, std::chrono::seconds(5),
                                   &uni_done);
    });
    fiber::async::spawn(group.at(0), [&gate, &cancelled]() { return cancel_bidi_after_delay(&gate, &cancelled); });

    ASSERT_EQ(bidi_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const AttachResult bidi_result = bidi_future.get();
    EXPECT_TRUE(cancelled.load(std::memory_order_relaxed));
    EXPECT_FALSE(bidi_result.ok);
    EXPECT_EQ(bidi_result.error, IoErr::Canceled);

    // The unidirectional waiter is untouched and still parked.
    EXPECT_EQ(uni_future.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
    EXPECT_EQ(gate.waiter_count(QuicStreamType::Unidirectional), 1U);
    EXPECT_EQ(gate.waiter_count(QuicStreamType::Bidirectional), 0U);

    fiber::async::spawn(group.at(0), [&gate]() { return cancel_all_on_loop(&gate); });
    ASSERT_EQ(uni_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(uni_future.get().error, IoErr::Canceled);

    group.stop();
    group.join();
}

// Fairness: a newcomer must not take credit that a queued request is waiting
// for, which is what lets the gate work without reserving anything.
TEST(QuicLocalStreamGateTest, TryAttachYieldsToQueuedWaiters) {
    fiber::event::EventLoopGroup group(1);
    QuicConnection::Options options = client_options(&group.at(0));
    options.max_local_bidirectional_streams = 0;
    QuicConnection conn(options);
    QuicLocalStreamGate gate(conn);
    wire_gate(conn, gate);
    ASSERT_TRUE(conn.mark_established());

    std::promise<AttachResult> done;
    auto future = done.get_future();
    auto queued_stream = make_test_stream();
    ASSERT_TRUE(queued_stream);

    group.start();
    fiber::async::spawn(group.at(0), [&gate, stream = std::move(queued_stream), &done]() mutable {
        return attach_through_gate(&gate, std::move(stream), QuicStreamType::Bidirectional, std::chrono::seconds(5),
                                   &done);
    });

    // Let the waiter park before granting credit it must not lose.
    std::promise<IoErr> barge;
    std::promise<std::size_t> parked;
    auto barge_future = barge.get_future();
    auto parked_future = parked.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        co_await fiber::async::sleep(std::chrono::milliseconds(20));
        parked.set_value(gate.waiter_count(QuicStreamType::Bidirectional));

        fiber::quic::QuicMaxStreamsFrame frame{};
        frame.bidirectional = true;
        frame.limit = 1;
        (void) conn.recv_max_streams_frame(frame);

        // Credit exists, but it belongs to the queued request.
        auto latecomer = make_test_stream();
        auto attached = gate.try_attach(std::move(latecomer), QuicStreamType::Bidirectional);
        barge.set_value(attached ? IoErr::None : attached.error());
    });

    ASSERT_EQ(barge_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(parked_future.get(), 1U);
    EXPECT_EQ(barge_future.get(), IoErr::Busy);

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const AttachResult result = future.get();
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.stream_id, 0U);

    group.stop();
    group.join();
}

TEST(QuicLocalStreamGateTest, AttachTimesOutWithoutCredit) {
    fiber::event::EventLoopGroup group(1);
    QuicConnection::Options options = client_options(&group.at(0));
    options.max_local_bidirectional_streams = 0;
    QuicConnection conn(options);
    QuicLocalStreamGate gate(conn);
    wire_gate(conn, gate);
    ASSERT_TRUE(conn.mark_established());

    std::promise<AttachResult> done;
    auto future = done.get_future();
    auto stream = make_test_stream();
    ASSERT_TRUE(stream);

    group.start();
    fiber::async::spawn(group.at(0), [&gate, stream = std::move(stream), &done]() mutable {
        return attach_through_gate(&gate, std::move(stream), QuicStreamType::Bidirectional,
                                   std::chrono::milliseconds(20), &done);
    });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const AttachResult result = future.get();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, IoErr::TimedOut);
    EXPECT_FALSE(gate.has_waiters());

    group.stop();
    group.join();
}

// A replay-safe caller wants 0-RTT or an answer. Waiting for the handshake
// would defeat the point, so it is told Busy instead of being parked.
TEST(QuicLocalStreamGateTest, ReplaySafeAttachDoesNotWaitForHandshake) {
    fiber::event::EventLoopGroup group(1);
    QuicConnection conn(client_options(&group.at(0)));
    QuicLocalStreamGate gate(conn);
    wire_gate(conn, gate);
    ASSERT_TRUE(conn.start_handshake());

    std::promise<AttachResult> done;
    auto future = done.get_future();
    auto stream = make_test_stream();
    ASSERT_TRUE(stream);

    group.start();
    fiber::async::spawn(group.at(0),
                        [&gate, stream = std::move(stream), &done]() mutable -> fiber::async::DetachedTask {
                            auto result = co_await gate.attach(std::move(stream), QuicStreamType::Bidirectional,
                                                               std::chrono::seconds(5),
                                                               fiber::quic::QuicStreamEarlyDataMode::ReplaySafe);
                            done.set_value(to_attach_result(result));
                        });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    const AttachResult result = future.get();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, IoErr::Busy);
    EXPECT_FALSE(gate.has_waiters());

    group.stop();
    group.join();
}

namespace {

template<class T>
auto start_pending_task(fiber::async::Task<T> &task) {
    auto handle = task.operator co_await().handle;
    handle.promise().set_continuation(std::noop_coroutine());
    handle.resume();
    return handle;
}

void exercise_cancelled_waiter(bool cancel_head) {
    fiber::event::EventLoop loop;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto options = client_options(&loop);
        options.max_local_bidirectional_streams = 0;
        QuicConnection conn(options);
        QuicLocalStreamGate gate(conn);
        wire_gate(conn, gate);
        EXPECT_TRUE(conn.mark_established());
        auto first = gate.attach(make_test_stream(), QuicStreamType::Bidirectional);
        auto second = gate.attach(make_test_stream(), QuicStreamType::Bidirectional);
        auto third = gate.attach(make_test_stream(), QuicStreamType::Bidirectional);
        auto h1 = start_pending_task(first);
        auto h2 = start_pending_task(second);
        auto h3 = start_pending_task(third);
        EXPECT_EQ(gate.waiter_count(), 3U);
        EXPECT_TRUE(conn.recv_max_streams_frame({.limit = 2, .bidirectional = true}));
        // Destroy a waiter already assigned credit, before any posted resume runs.
        if (cancel_head) {
            first = {};
        } else {
            second = {};
        }
        co_await fiber::async::sleep(std::chrono::milliseconds(10));
        auto survivor = cancel_head ? h2 : h1;
        EXPECT_TRUE(survivor.done());
        EXPECT_TRUE(h3.done());
        if (survivor.done() && h3.done()) {
            auto earlier = survivor.promise().result();
            auto later = h3.promise().result();
            EXPECT_TRUE(earlier);
            EXPECT_TRUE(later);
            if (earlier && later) {
                EXPECT_EQ((*earlier)->stream_id(), 0U);
                EXPECT_EQ((*later)->stream_id(), 4U);
            }
        }
        EXPECT_EQ(gate.waiter_count(), 0U);
        loop.stop();
    });
    loop.run();
}

} // namespace

TEST(QuicLocalStreamGateTest, DestroyingSignaledHeadRedistributesCreditInFifoOrder) { exercise_cancelled_waiter(true); }

TEST(QuicLocalStreamGateTest, DestroyingSignaledMiddleRedistributesCreditInFifoOrder) {
    exercise_cancelled_waiter(false);
}

TEST(QuicLocalStreamGateTest, CancelAllOverridesQueuedSignalsWithoutAttachingStreams) {
    fiber::event::EventLoop loop;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto options = client_options(&loop);
        options.max_local_bidirectional_streams = 0;
        QuicConnection conn(options);
        QuicLocalStreamGate gate(conn);
        wire_gate(conn, gate);
        EXPECT_TRUE(conn.mark_established());
        auto first = gate.attach(make_test_stream(), QuicStreamType::Bidirectional);
        auto second = gate.attach(make_test_stream(), QuicStreamType::Bidirectional);
        auto h1 = start_pending_task(first);
        auto h2 = start_pending_task(second);
        EXPECT_TRUE(conn.recv_max_streams_frame({.limit = 2, .bidirectional = true}));
        gate.cancel_all(IoErr::Canceled);
        EXPECT_FALSE(gate.has_waiters());
        co_await fiber::async::sleep(std::chrono::milliseconds(10));
        EXPECT_TRUE(h1.done());
        EXPECT_TRUE(h2.done());
        if (h1.done() && h2.done()) {
            EXPECT_EQ(h1.promise().result().error(), IoErr::Canceled);
            EXPECT_EQ(h2.promise().result().error(), IoErr::Canceled);
        }
        EXPECT_EQ(conn.active_stream_count(), 0U);
        loop.stop();
    });
    loop.run();
}

TEST(QuicLocalStreamGateTest, TimedOutHeadRedistributesCreditBeforeItsQueuedResume) {
    fiber::event::EventLoop loop;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto options = client_options(&loop);
        options.max_local_bidirectional_streams = 0;
        QuicConnection conn(options);
        QuicLocalStreamGate gate(conn);
        wire_gate(conn, gate);
        EXPECT_TRUE(conn.mark_established());
        // Expire both timers in one loop turn, with credit granted first.
        // The timeout must replace the queued signal and transfer its credit.
        struct Grant {
            fiber::event::EventLoop::TimerEntry timer{};
            QuicConnection *conn;
            static void fire(Grant *self) noexcept {
                EXPECT_TRUE(self->conn->recv_max_streams_frame({.limit = 1, .bidirectional = true}));
            }
        } grant{.conn = &conn};
        const auto deadline = loop.now() + std::chrono::milliseconds(20);
        loop.post_at<Grant, &Grant::timer, &Grant::fire>(deadline - std::chrono::milliseconds(1), grant);
        auto first = gate.attach(make_test_stream(), QuicStreamType::Bidirectional, std::chrono::milliseconds(20));
        auto second = gate.attach(make_test_stream(), QuicStreamType::Bidirectional);
        auto h1 = start_pending_task(first);
        auto h2 = start_pending_task(second);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        co_await fiber::async::sleep(std::chrono::milliseconds(30));
        EXPECT_TRUE(h1.done());
        EXPECT_TRUE(h2.done());
        if (h1.done() && h2.done()) {
            EXPECT_EQ(h1.promise().result().error(), IoErr::TimedOut);
            EXPECT_TRUE(h2.promise().result());
        }
        loop.stop();
    });
    loop.run();
}

TEST(QuicLocalStreamGateTest, DestroyingGateCancelsQueuedResumesWithoutAccessingDestroyedOwner) {
    fiber::event::EventLoop loop;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto options = client_options(&loop);
        options.max_local_bidirectional_streams = 0;
        QuicConnection conn(options);
        auto gate = std::make_unique<QuicLocalStreamGate>(conn);
        wire_gate(conn, *gate);
        EXPECT_TRUE(conn.mark_established());
        auto first = gate->attach(make_test_stream(), QuicStreamType::Bidirectional);
        auto second = gate->attach(make_test_stream(), QuicStreamType::Bidirectional);
        auto h1 = start_pending_task(first);
        auto h2 = start_pending_task(second);
        EXPECT_TRUE(conn.recv_max_streams_frame({.limit = 1, .bidirectional = true}));
        gate.reset();
        EXPECT_TRUE(conn.set_app_ops(nullptr, {}));
        co_await fiber::async::sleep(std::chrono::milliseconds(10));
        EXPECT_TRUE(h1.done());
        EXPECT_TRUE(h2.done());
        if (h1.done() && h2.done()) {
            EXPECT_EQ(h1.promise().result().error(), IoErr::Canceled);
            EXPECT_EQ(h2.promise().result().error(), IoErr::Canceled);
        }
        EXPECT_EQ(conn.active_stream_count(), 0U);
        loop.stop();
    });
    loop.run();
}
