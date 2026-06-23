#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <string_view>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "event/EventLoopGroup.h"
#include "quic/QuicConnection.h"

namespace {

fiber::quic::QuicSlice slice_of(std::string_view value) {
    return {reinterpret_cast<const std::uint8_t *>(value.data()), value.size()};
}

struct ShutdownCallbackState {
    std::uint32_t schedule_calls = 0;
    std::uint32_t idle_timeout_calls = 0;
    std::uint32_t close_timeout_calls = 0;
};

fiber::quic::QuicStream::Lease make_test_stream(const fiber::quic::QuicNewStreamContext &ctx) noexcept {
    return fiber::quic::QuicStream::Lease::adopt(
            new fiber::quic::QuicStream(ctx.stream_id, ctx.recv_extent_pool, ctx.recv_options));
}

fiber::quic::QuicStream::Lease on_new_stream(void * /*owner*/, const fiber::quic::QuicNewStreamContext &ctx) noexcept {
    return make_test_stream(ctx);
}

void schedule_send_record(void *owner, fiber::quic::QuicConnection &) noexcept {
    ++static_cast<ShutdownCallbackState *>(owner)->schedule_calls;
}

void idle_timeout_record(void *owner, fiber::quic::QuicConnection &) noexcept {
    ++static_cast<ShutdownCallbackState *>(owner)->idle_timeout_calls;
}

void close_timeout_record(void *owner, fiber::quic::QuicConnection &) noexcept {
    ++static_cast<ShutdownCallbackState *>(owner)->close_timeout_calls;
}

fiber::quic::QuicConnection::Options established_server_options(ShutdownCallbackState &state) noexcept {
    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.owner = &state;
    options.ops.on_new_stream = on_new_stream;
    options.schedule_send_owner = &state;
    options.schedule_send = schedule_send_record;
    options.lifecycle_owner = &state;
    options.on_idle_timeout = idle_timeout_record;
    options.on_close_timeout = close_timeout_record;
    // Don't let idle timeout interfere with graceful tests by default.
    options.transport.max_idle_timeout = std::chrono::seconds(60);
    return options;
}

std::size_t count_pending_frame_type(const fiber::quic::QuicConnection &conn, fiber::quic::QuicEncryptionLevel level,
                                     fiber::quic::QuicFrameType type) {
    const auto &space = conn.packet_number_space(level);
    std::size_t count = 0;
    for (const fiber::quic::QuicOutputFrame *frame = space.pending_frames.front(); frame != nullptr;
         frame = space.pending_frames.next_of(*frame)) {
        if (frame->type == type) {
            ++count;
        }
    }
    return count;
}

const fiber::quic::QuicOutputFrame *find_pending_frame_of_type(const fiber::quic::QuicConnection &conn,
                                                               fiber::quic::QuicEncryptionLevel level,
                                                               fiber::quic::QuicFrameType type) {
    const auto &space = conn.packet_number_space(level);
    for (const fiber::quic::QuicOutputFrame *frame = space.pending_frames.front(); frame != nullptr;
         frame = space.pending_frames.next_of(*frame)) {
        if (frame->type == type) {
            return frame;
        }
    }
    return nullptr;
}

// Drive an Established + Application-write-ready connection so close() can
// actually queue a CC frame at the Application level.
void mark_established_with_app_keys(fiber::quic::QuicConnection &conn) {
    ASSERT_TRUE(conn.mark_established().has_value());
    conn.crypto().application_write.ready = true;
}

// Open a peer-initiated stream by feeding a STREAM frame with FIN, then drain
// it. The stream stays attached until release_stream_app() is called.
fiber::quic::QuicStream *open_peer_stream(fiber::quic::QuicConnection &conn, std::uint64_t stream_id) {
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = stream_id;
    frame.length = 3;
    frame.fin = true;
    auto received = conn.recv_stream_frame(frame, slice_of("abc"));
    if (!received) {
        return nullptr;
    }
    auto *stream = conn.find_stream(stream_id);
    if (stream == nullptr) {
        return nullptr;
    }
    fiber::mem::IoBufChain out(conn.recv_extent_pool());
    auto taken = stream->try_read(3, out);
    if (!taken || *taken != 3) {
        return nullptr;
    }
    return stream;
}

} // namespace

// Test 1 — Shutdown with no in-flight streams transitions straight to Closing
// and queues an Application-level CONNECTION_CLOSE.
TEST(QuicConnectionShutdownTest, ShutdownWithoutStreamsClosesImmediately) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    conn.shutdown(fiber::quic::QuicErrorCode::NoError);

    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Closing);
    EXPECT_FALSE(conn.shutting_down());
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ConnectionClose),
              1U);
}

// Test 2 — Last in-flight stream retiring while shutdown_pending triggers the
// final close.
TEST(QuicConnectionShutdownTest, LastStreamRetirementFinalizesShutdown) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    auto *stream = open_peer_stream(conn, 0);
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(conn.active_stream_count(), 1U);

    conn.shutdown(fiber::quic::QuicErrorCode::NoError);
    EXPECT_TRUE(conn.shutting_down());
    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::GracefulClosing);

    conn.release_stream_app(*stream);

    EXPECT_FALSE(conn.shutting_down());
    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Closing);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ConnectionClose),
              1U);
}

namespace {

fiber::async::DetachedTask run_grace_timer(fiber::quic::QuicConnection *conn, std::chrono::milliseconds grace,
                                           std::chrono::milliseconds wait,
                                           std::promise<fiber::quic::QuicConnectionState> *done) {
    conn->shutdown(fiber::quic::QuicErrorCode::NoError, /*frame_type=*/0, grace);
    co_await fiber::async::sleep(wait);
    done->set_value(conn->state());
    fiber::event::EventLoop::current().stop();
}

} // namespace

// Test 3 — Grace timer expiring forces the connection into Closing even when
// streams are still attached.
TEST(QuicConnectionShutdownTest, GraceTimerForcesCloseWhenStreamsRemain) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    auto *stream = open_peer_stream(conn, 0);
    ASSERT_NE(stream, nullptr);

    std::promise<fiber::quic::QuicConnectionState> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_grace_timer(&conn, std::chrono::milliseconds(15), std::chrono::milliseconds(60), &done);
    });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(future.get(), fiber::quic::QuicConnectionState::Closing);
    EXPECT_FALSE(conn.shutting_down());

    group.join();
}

// Test 4 — Receiving a CONNECTION_CLOSE while in graceful shutdown switches
// the connection to Draining and leaves graceful shutdown state.
TEST(QuicConnectionShutdownTest, ReceivingCloseDuringShutdownEntersDraining) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    auto *stream = open_peer_stream(conn, 0);
    ASSERT_NE(stream, nullptr);

    conn.shutdown(fiber::quic::QuicErrorCode::NoError);
    ASSERT_TRUE(conn.shutting_down());

    conn.begin_draining(fiber::quic::QuicErrorCode::InternalError);

    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Draining);
    EXPECT_FALSE(conn.shutting_down());
    EXPECT_FALSE(conn.close_timer_armed());
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ConnectionClose),
              0U);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::StopSending),
              0U);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ResetStream),
              0U);
}

// Test 5 — close_immediately() taking over a graceful shutdown clears the
// flag and accelerates the timer.
TEST(QuicConnectionShutdownTest, CloseImmediatelyTakesOverGracefulShutdown) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    auto *stream = open_peer_stream(conn, 0);
    ASSERT_NE(stream, nullptr);

    conn.shutdown(fiber::quic::QuicErrorCode::NoError);
    ASSERT_TRUE(conn.shutting_down());

    conn.close_immediately(fiber::quic::QuicErrorCode::InternalError);

    EXPECT_FALSE(conn.shutting_down());
    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Closing);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ConnectionClose),
              1U);
}

TEST(QuicConnectionShutdownTest, CloseTakesOverGracefulShutdownWithoutStreamControlFrames) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    auto *stream = open_peer_stream(conn, 0);
    ASSERT_NE(stream, nullptr);

    conn.shutdown(fiber::quic::QuicErrorCode::NoError);
    ASSERT_EQ(conn.state(), fiber::quic::QuicConnectionState::GracefulClosing);

    conn.close(fiber::quic::QuicErrorCode::InternalError);

    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Closing);
    EXPECT_FALSE(conn.shutting_down());
    EXPECT_EQ(conn.close_error(), fiber::quic::QuicErrorCode::InternalError);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ConnectionClose),
              1U);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::StopSending),
              0U);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ResetStream),
              0U);
}

namespace {

fiber::async::DetachedTask run_idle_timeout_during_shutdown(fiber::quic::QuicConnection *conn,
                                                            std::chrono::milliseconds wait,
                                                            std::promise<fiber::quic::QuicConnectionState> *done) {
    conn->shutdown(fiber::quic::QuicErrorCode::NoError, /*frame_type=*/0, std::chrono::seconds(5));
    conn->arm_idle_timer(fiber::event::EventLoop::current());
    co_await fiber::async::sleep(wait);
    done->set_value(conn->state());
    fiber::event::EventLoop::current().stop();
}

} // namespace

// Test 6 — Idle timeout firing while shutdown is pending discards graceful
// state and goes straight to Closed.
TEST(QuicConnectionShutdownTest, IdleTimeoutDuringShutdownGoesToClosed) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    options.transport.max_idle_timeout = std::chrono::milliseconds(10);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    auto *stream = open_peer_stream(conn, 0);
    ASSERT_NE(stream, nullptr);

    std::promise<fiber::quic::QuicConnectionState> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_idle_timeout_during_shutdown(&conn, std::chrono::milliseconds(40), &done);
    });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(future.get(), fiber::quic::QuicConnectionState::Closed);
    EXPECT_FALSE(conn.shutting_down());
    EXPECT_EQ(state.idle_timeout_calls, 1U);

    group.join();
}

// Test 7 — During graceful shutdown, new peer-initiated streams are refused
// via the can_accept_peer_stream gate.
TEST(QuicConnectionShutdownTest, PeerStreamRejectedDuringShutdown) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    auto *stream = open_peer_stream(conn, 0);
    ASSERT_NE(stream, nullptr);

    conn.shutdown(fiber::quic::QuicErrorCode::NoError);
    ASSERT_TRUE(conn.shutting_down());

    EXPECT_FALSE(conn.can_accept_peer_stream(4));
    EXPECT_FALSE(conn.accepting_new_streams());

    auto created = conn.get_or_create_peer_stream(4);
    EXPECT_FALSE(created.has_value());
    EXPECT_EQ(created.error(), fiber::common::IoErr::Canceled);
}

// Test 8 — During graceful shutdown, next_local_stream_id() returns Canceled.
TEST(QuicConnectionShutdownTest, LocalStreamRequestRejectedDuringShutdown) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    // Pin a peer stream so shutdown() stays in the shutdown_pending state
    // rather than finalising immediately.
    auto *stream = open_peer_stream(conn, 0);
    ASSERT_NE(stream, nullptr);

    // Servers allocate unidirectional stream ids starting at 3 (0b11).
    auto first = conn.next_local_stream_id(fiber::quic::QuicStreamType::Unidirectional);
    ASSERT_TRUE(first.has_value());

    conn.shutdown(fiber::quic::QuicErrorCode::NoError);
    ASSERT_TRUE(conn.shutting_down());

    auto second = conn.next_local_stream_id(fiber::quic::QuicStreamType::Unidirectional);
    EXPECT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), fiber::common::IoErr::Canceled);
}

namespace {

fiber::async::DetachedTask run_keepalive_during_shutdown(fiber::quic::QuicConnection *conn,
                                                         std::chrono::milliseconds wait,
                                                         std::promise<std::size_t> *done) {
    conn->shutdown(fiber::quic::QuicErrorCode::NoError);
    conn->arm_keepalive_timer(fiber::event::EventLoop::current());
    co_await fiber::async::sleep(wait);
    done->set_value(count_pending_frame_type(*conn, fiber::quic::QuicEncryptionLevel::Application,
                                             fiber::quic::QuicFrameType::Ping));
    fiber::event::EventLoop::current().stop();
}

} // namespace

// Test 9 — arm_keepalive_timer() while GracefulClosing is a no-op; even if the
// timer were already in flight, on_keepalive_timer suppresses the PING.
TEST(QuicConnectionShutdownTest, KeepaliveSuppressedDuringShutdown) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    options.keepalive_interval = std::chrono::milliseconds(5);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    // Pin a peer stream to keep the connection out of immediate close.
    auto *stream = open_peer_stream(conn, 0);
    ASSERT_NE(stream, nullptr);

    std::promise<std::size_t> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_keepalive_during_shutdown(&conn, std::chrono::milliseconds(40), &done); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(future.get(), 0U);
    EXPECT_FALSE(conn.keepalive_timer_armed());

    group.join();
}

// Test 11 — shutdown_application() emits an Application-level CONNECTION_CLOSE_APP
// and the application error code is preserved.
TEST(QuicConnectionShutdownTest, ShutdownApplicationProducesAppCloseFrame) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    constexpr std::uint64_t kAppError = 0x108; // H3_INTERNAL_ERROR
    conn.shutdown_application(kAppError);

    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Closing);
    EXPECT_FALSE(conn.shutting_down());

    const auto *frame = find_pending_frame_of_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                                   fiber::quic::QuicFrameType::ConnectionCloseApp);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->u.close.error_code, kAppError);
}

// Test 12 — A second shutdown() is a no-op: the staged error and grace timer
// are preserved.
TEST(QuicConnectionShutdownTest, RepeatedShutdownIsNoOp) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    auto *stream = open_peer_stream(conn, 0);
    ASSERT_NE(stream, nullptr);

    conn.shutdown(fiber::quic::QuicErrorCode::InternalError);
    ASSERT_TRUE(conn.shutting_down());
    ASSERT_EQ(conn.close_error(), fiber::quic::QuicErrorCode::InternalError);

    // Second call with a different error must not overwrite the staged state.
    conn.shutdown(fiber::quic::QuicErrorCode::ProtocolViolation);
    EXPECT_TRUE(conn.shutting_down());
    EXPECT_EQ(conn.close_error(), fiber::quic::QuicErrorCode::InternalError);

    // Finalize and verify the original error is what gets serialised.
    conn.release_stream_app(*stream);
    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Closing);
    const auto *frame = find_pending_frame_of_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                                   fiber::quic::QuicFrameType::ConnectionClose);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->u.close.error_code, static_cast<std::uint64_t>(fiber::quic::QuicErrorCode::InternalError));
}

// Test 13 — shutdown() with no active streams closes immediately even while
// handshaking; there is no stream work to wait for.
TEST(QuicConnectionShutdownTest, ShutdownDuringHandshakingWithNoStreamsClosesImmediately) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    ASSERT_TRUE(conn.start_handshake().has_value());
    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Handshaking);

    conn.shutdown(fiber::quic::QuicErrorCode::NoError);
    EXPECT_FALSE(conn.shutting_down());
    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Closing);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ConnectionClose),
              0U);
}

TEST(QuicConnectionShutdownTest, StreamCloseQueuesControlFramesBeforeConnectionClosing) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0;
    frame.length = 3;
    ASSERT_TRUE(conn.recv_stream_frame(frame, slice_of("abc")).has_value());
    auto *stream = conn.find_stream(0);
    ASSERT_NE(stream, nullptr);
    fiber::mem::IoBufChain out(conn.recv_extent_pool());
    ASSERT_TRUE(stream->try_read(3, out).has_value());

    stream->close(7);

    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::StopSending),
              1U);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ResetStream),
              1U);
}

TEST(QuicConnectionShutdownTest, ConnectionCloseDoesNotQueueStreamControlFrames) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    auto *stream = open_peer_stream(conn, 0);
    ASSERT_NE(stream, nullptr);

    conn.close(fiber::quic::QuicErrorCode::InternalError);

    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Closing);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ConnectionClose),
              1U);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::StopSending),
              0U);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ResetStream),
              0U);
}
