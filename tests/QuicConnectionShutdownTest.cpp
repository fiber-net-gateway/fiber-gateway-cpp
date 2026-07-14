#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <new>
#include <string_view>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "event/EventLoopGroup.h"
#include "quic/QuicConnection.h"
#include "quic/QuicCrypto.h"
#include "quic/QuicPacketProcessor.h"

#include "QuicTestLoop.h"

namespace {

fiber::quic::QuicSlice slice_of(std::string_view value) {
    return {reinterpret_cast<const std::uint8_t *>(value.data()), value.size()};
}

struct ShutdownCallbackState {};

void destroy_test_stream(void *, fiber::quic::QuicStream &stream) noexcept { delete &stream; }

fiber::quic::QuicStream::Lease create_stream(void * /*owner*/, std::uint64_t /*stream_id*/) noexcept {
    return fiber::quic::QuicStream::Lease::adopt(new (std::nothrow)
                                                         fiber::quic::QuicStream(nullptr, destroy_test_stream));
}

fiber::quic::QuicConnection::Options established_server_options(ShutdownCallbackState &state) noexcept {
    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.owner = &state;
    options.ops.create_stream = create_stream;
    // Don't let idle timeout interfere with graceful tests by default.
    options.transport.max_idle_timeout = std::chrono::seconds(60);
    // Default to the shared non-running test loop; callers that drive
    // coroutines/timers override this with their running group loop.
    options.loop = &fiber::test::quic_loop();
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

// Open a peer-initiated stream by feeding a STREAM frame with FIN. The data is
// left buffered (not consumed), so the stream stays attached and active until
// something retires it (close() / consuming the data / a RESET).
fiber::quic::QuicStream *open_peer_stream(fiber::quic::QuicConnection &conn, std::uint64_t stream_id) {
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = stream_id;
    frame.length = 3;
    frame.fin = true;
    auto received = conn.recv_stream_frame(frame, slice_of("abc"));
    if (!received) {
        return nullptr;
    }
    return conn.find_stream(stream_id);
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

    stream->close();

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
    options.loop = &group.at(0);
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
    options.loop = &group.at(0);
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

namespace {

// Snapshot of connection state taken synchronously after retiring the last
// stream, before the loop is pumped. Used to prove the GracefulClosing ->
// Closing transition is deferred out of the frame loop (audit #3).
struct DeferredCloseSnapshot {
    fiber::quic::QuicConnectionState state{};
    bool close_timer_armed = false;
};

fiber::async::DetachedTask run_graceful_close_completion(fiber::quic::QuicConnection *conn,
                                                         fiber::quic::QuicStream *stream,
                                                         std::promise<DeferredCloseSnapshot> *before_pump,
                                                         std::promise<fiber::quic::QuicConnectionState> *after_pump) {
    // Retiring the last stream from the running loop reaches
    // maybe_finish_graceful_close() with current_or_null() == loop, so the
    // Closing transition must be deferred (audit #3): state stays GracefulClosing
    // and the close timer is armed for "now".
    stream->close();
    before_pump->set_value(DeferredCloseSnapshot{conn->state(), conn->close_timer_armed()});
    // Pump the loop so the deferred on_close_timer fires enter_closing().
    co_await fiber::async::sleep(std::chrono::milliseconds(2));
    after_pump->set_value(conn->state());
    fiber::event::EventLoop::current().stop();
}

} // namespace

// Test - Retiring the last in-flight stream from the running event loop defers
// the GracefulClosing -> Closing transition to the next tick (audit #3
// re-entrancy fix). Before the fix, enter_closing() ran inline mid-frame-loop,
// transitioning to Closing synchronously; nginx defers equivalently via
// ngx_post_event(&qc->close, ...).
TEST(QuicConnectionShutdownTest, GracefulCloseCompletionDeferredOnRunningLoop) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    options.loop = &group.at(0);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    auto *stream = open_peer_stream(conn, 0);
    ASSERT_NE(stream, nullptr);
    conn.shutdown(fiber::quic::QuicErrorCode::NoError);
    ASSERT_EQ(conn.state(), fiber::quic::QuicConnectionState::GracefulClosing);

    std::promise<DeferredCloseSnapshot> before_pump;
    std::promise<fiber::quic::QuicConnectionState> after_pump;
    auto before_future = before_pump.get_future();
    auto after_future = after_pump.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_graceful_close_completion(&conn, stream, &before_pump, &after_pump); });

    ASSERT_EQ(before_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const DeferredCloseSnapshot before = before_future.get();
    // Deferred: retiring the last stream did NOT synchronously enter Closing.
    EXPECT_EQ(before.state, fiber::quic::QuicConnectionState::GracefulClosing);
    EXPECT_TRUE(before.close_timer_armed);

    ASSERT_EQ(after_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    // After pumping the loop, the deferred transition fires.
    EXPECT_EQ(after_future.get(), fiber::quic::QuicConnectionState::Closing);
    EXPECT_FALSE(conn.shutting_down());
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ConnectionClose),
              1U);

    group.join();
}

// Test 9 — arm_keepalive_timer() while GracefulClosing is a no-op; even if the
// timer were already in flight, on_keepalive_timer suppresses the PING.
TEST(QuicConnectionShutdownTest, KeepaliveSuppressedDuringShutdown) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    options.loop = &group.at(0);
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
    stream->close();
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

// RFC 9000 §20.1 — a TLS handshake alert must be reported to the peer as a
// CRYPTO_ERROR transport code (0x0100 | alert). Verify the mapping helper in
// isolation across the alert range, including the special alerts nginx
// special-cases (no_application_protocol=120, missing_extension=109), which the
// general 0x0100 | alert encoding already covers.
TEST(QuicConnectionShutdownTest, CryptoErrorCodeMapsAlertToTransportError) {
    EXPECT_EQ(fiber::quic::quic_crypto_error_code(0), 0x0100u);
    EXPECT_EQ(fiber::quic::quic_crypto_error_code(40), 0x0128u); // handshake_failure
    EXPECT_EQ(fiber::quic::quic_crypto_error_code(109), 0x016Du); // missing_extension
    EXPECT_EQ(fiber::quic::quic_crypto_error_code(120), 0x0178u); // no_application_protocol
    EXPECT_EQ(fiber::quic::quic_crypto_error_code(255), 0x01FFu);
}

namespace {

// Runs close_crypto_error() on the event loop so the close path observes a
// current EventLoop (required for schedule_send() to fire) and signals
// completion. Mirrors the run_grace_timer harness above.
fiber::async::DetachedTask run_crypto_error_close(fiber::quic::QuicConnection *conn, std::uint8_t alert,
                                                  std::promise<void> *done) {
    conn->close_crypto_error(alert);
    done->set_value();
    fiber::event::EventLoop::current().stop();
    co_return;
}

} // namespace

// RFC 9000 §20.1 — a TLS alert raised during the handshake drives the connection
// into a terminal closing state with a CONNECTION_CLOSE carrying CRYPTO_ERROR
// (0x0100 | alert), queued on an encryption level with write keys and actually
// scheduled for send (not merely staged). Uses no_application_protocol
// (alert 120) -> 0x0178. Asserts after group.join() so the loop thread is fully
// stopped and connection state is stable (terminal_closing covers both Closing
// and the immediate-timer transition to Closed).
TEST(QuicConnectionShutdownTest, TlsAlertCloseStagesCryptoErrorAndSchedulesSend) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    options.loop = &group.at(0);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_crypto_error_close(&conn, 120, &done); });

    ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    future.get();
    group.join();

    EXPECT_TRUE(conn.terminal_closing());
    EXPECT_EQ(static_cast<std::uint64_t>(conn.close_error()), 0x0100u | 120u);

    const auto *cc = find_pending_frame_of_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                                fiber::quic::QuicFrameType::ConnectionClose);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->u.close.error_code, 0x0100u | 120u);
    EXPECT_EQ(cc->u.close.frame_type, 0u);

    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ConnectionClose),
              1U);
}

// === RFC 9000 §4.1 / §4.6: precise transport error on flow-control / stream
// violations. A misbehaving peer MUST close the connection with the exact
// transport error code (and the offending frame type stamped into the
// CONNECTION_CLOSE frame), rather than silently dropping the datagram. These
// tests mirror nginx's ngx_event_quic_streams.c close sites.

// STREAM data exceeding the per-stream max_stream_data limit → FLOW_CONTROL_ERROR.
TEST(QuicConnectionShutdownTest, StreamDataExceedingMaxStreamDataClosesFlowControl) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    options.recv_flow.stream_buffer_limit = 8; // advertised max_stream_data
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0;
    frame.length = 9; // end offset 9 > max_stream_data 8
    auto received = conn.recv_stream_frame(frame, slice_of("123456789"));

    EXPECT_FALSE(received.has_value());
    EXPECT_TRUE(conn.terminal_closing());
    EXPECT_EQ(conn.close_error(), fiber::quic::QuicErrorCode::FlowControlError);

    const auto *cc = find_pending_frame_of_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                                fiber::quic::QuicFrameType::ConnectionClose);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->u.close.error_code, static_cast<std::uint64_t>(fiber::quic::QuicErrorCode::FlowControlError));
    EXPECT_EQ(cc->u.close.frame_type, static_cast<std::uint64_t>(fiber::quic::QuicFrameType::Stream));
}

// STREAM data exceeding the connection-level max_data limit → FLOW_CONTROL_ERROR.
TEST(QuicConnectionShutdownTest, StreamDataExceedingMaxDataClosesFlowControl) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    options.recv_flow.conn_recv_limit = 10;
    options.recv_flow.conn_recv_low_water = 0;
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0;
    frame.length = 11; // end offset 11 > connection max_data 10
    auto received = conn.recv_stream_frame(frame, slice_of("xxxxxxxxxxx"));

    EXPECT_FALSE(received.has_value());
    EXPECT_TRUE(conn.terminal_closing());
    EXPECT_EQ(conn.close_error(), fiber::quic::QuicErrorCode::FlowControlError);

    const auto *cc = find_pending_frame_of_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                                fiber::quic::QuicFrameType::ConnectionClose);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->u.close.frame_type, static_cast<std::uint64_t>(fiber::quic::QuicFrameType::Stream));
}

// RESET_STREAM final size smaller than data already received → FINAL_SIZE_ERROR.
TEST(QuicConnectionShutdownTest, ResetStreamFinalSizeBelowReceivedClosesFinalSize) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    fiber::quic::QuicStreamFrame data{};
    data.stream_id = 0;
    data.offset = 0;
    data.has_offset = true;
    data.length = 10;
    ASSERT_TRUE(conn.recv_stream_frame(data, slice_of("0123456789")).has_value());

    fiber::quic::QuicResetStreamFrame reset{};
    reset.id = 0;
    reset.final_size = 5; // 5 < 10 already received
    auto received = conn.recv_reset_stream_frame(reset);

    EXPECT_FALSE(received.has_value());
    EXPECT_TRUE(conn.terminal_closing());
    EXPECT_EQ(conn.close_error(), fiber::quic::QuicErrorCode::FinalSizeError);

    const auto *cc = find_pending_frame_of_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                                fiber::quic::QuicFrameType::ConnectionClose);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->u.close.error_code, static_cast<std::uint64_t>(fiber::quic::QuicErrorCode::FinalSizeError));
    EXPECT_EQ(cc->u.close.frame_type, static_cast<std::uint64_t>(fiber::quic::QuicFrameType::ResetStream));
}

// STREAM frame with FIN whose final size is smaller than data already received
// → FINAL_SIZE_ERROR (the STREAM-frame FIN path).
TEST(QuicConnectionShutdownTest, StreamFinFinalSizeBelowReceivedClosesFinalSize) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    fiber::quic::QuicStreamFrame data{};
    data.stream_id = 0;
    data.offset = 0;
    data.has_offset = true;
    data.length = 10;
    ASSERT_TRUE(conn.recv_stream_frame(data, slice_of("0123456789")).has_value());

    fiber::quic::QuicStreamFrame fin{};
    fin.stream_id = 0;
    fin.length = 5; // final size 5 < 10 already received
    fin.fin = true;
    auto received = conn.recv_stream_frame(fin, slice_of("abcde"));

    EXPECT_FALSE(received.has_value());
    EXPECT_TRUE(conn.terminal_closing());
    EXPECT_EQ(conn.close_error(), fiber::quic::QuicErrorCode::FinalSizeError);

    const auto *cc = find_pending_frame_of_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                                fiber::quic::QuicFrameType::ConnectionClose);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->u.close.frame_type, static_cast<std::uint64_t>(fiber::quic::QuicFrameType::Stream));
}

// STOP_SENDING on a peer-initiated unidirectional stream (peer is the sender,
// so it cannot tell us to stop sending) → STREAM_STATE_ERROR.
TEST(QuicConnectionShutdownTest, StopSendingOnPeerUnidirectionalClosesStreamState) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    // Open a peer-initiated unidirectional stream (client-uni, id 2): the peer
    // is the sender, the local side is the receiver.
    fiber::quic::QuicStreamFrame open{};
    open.stream_id = 2;
    ASSERT_TRUE(conn.recv_stream_frame(open, {}).has_value());

    fiber::quic::QuicStopSendingFrame stop{};
    stop.id = 2;
    auto received = conn.recv_stop_sending_frame(stop);

    EXPECT_FALSE(received.has_value());
    EXPECT_TRUE(conn.terminal_closing());
    EXPECT_EQ(conn.close_error(), fiber::quic::QuicErrorCode::StreamStateError);

    const auto *cc = find_pending_frame_of_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                                fiber::quic::QuicFrameType::ConnectionClose);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->u.close.error_code, static_cast<std::uint64_t>(fiber::quic::QuicErrorCode::StreamStateError));
    EXPECT_EQ(cc->u.close.frame_type, static_cast<std::uint64_t>(fiber::quic::QuicFrameType::StopSending));
}

// A peer stream ID at or beyond the advertised max_streams → STREAM_LIMIT_ERROR.
// (Distinct from the concurrent-active-stream window, which is a non-fatal gate.)
TEST(QuicConnectionShutdownTest, PeerStreamExceedingAdvertisedMaxStreamsClosesStreamLimit) {
    ShutdownCallbackState state{};
    auto options = established_server_options(state);
    options.max_peer_bidirectional_streams = 0; // advertise zero bidi streams
    fiber::quic::QuicConnection conn(options);
    mark_established_with_app_keys(conn);

    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = 0; // seq 0 >= advertised 0
    auto received = conn.recv_stream_frame(frame, {});

    EXPECT_FALSE(received.has_value());
    EXPECT_TRUE(conn.terminal_closing());
    EXPECT_EQ(conn.close_error(), fiber::quic::QuicErrorCode::StreamLimitError);

    const auto *cc = find_pending_frame_of_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                                fiber::quic::QuicFrameType::ConnectionClose);
    ASSERT_NE(cc, nullptr);
    EXPECT_EQ(cc->u.close.error_code, static_cast<std::uint64_t>(fiber::quic::QuicErrorCode::StreamLimitError));
    EXPECT_EQ(cc->u.close.frame_type, static_cast<std::uint64_t>(fiber::quic::QuicFrameType::Stream));
}

// --- Stateless reset detection (RFC 9000 §10.3) ----------------------------

namespace {

fiber::quic::QuicConnectionId make_cid(const std::uint8_t *bytes, std::uint8_t len) {
    return fiber::quic::QuicConnectionId::from_bytes(bytes, len).value_or(fiber::quic::QuicConnectionId{});
}

// Server options seeded with a non-empty remote (peer initial source) CID so
// that remote_cids_[0] is active at sequence 0 with a zero stateless_reset_token
// (the initial CID carries no token — RFC 9000 §10.3.1).
fiber::quic::QuicConnection::Options reset_test_options(ShutdownCallbackState &state) {
    auto options = established_server_options(state);
    const std::uint8_t remote_cid[8] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
    options.remote_connection_id = make_cid(remote_cid, 8);
    return options;
}

// Install real Application READ keys (AES-128-GCM) so short-header decryption
// is actually attempted on forged packets and fails at the AEAD tag check — the
// realistic stateless-reset detection path. Flags next-generation keys ready so
// an inadvertent key-phase bit in the forged garbage cannot trigger a spurious
// KEY_UPDATE_ERROR close before detection runs.
void install_application_read_keys(fiber::quic::QuicConnection &conn) {
    constexpr fiber::quic::QuicCryptoSuite suite = fiber::quic::QuicCryptoSuite::Aes128GcmSha256;
    std::array<std::uint8_t, fiber::quic::kQuicMaxSecretLength> app_secret{};
    for (std::size_t i = 0; i < app_secret.size(); ++i) {
        app_secret[i] = static_cast<std::uint8_t>(0x5aU ^ static_cast<std::uint8_t>(i));
    }
    ASSERT_TRUE(fiber::quic::quic_set_encryption_secret(conn.crypto(), fiber::quic::QuicEncryptionLevel::Application,
                                                        /*write_secret=*/false, suite, app_secret.data(), 32));
    conn.crypto().next_application_keys_ready = true;
}

// Record a peer-issued NEW_CONNECTION_ID (carrying a stateless_reset_token) at
// the given sequence number.
void store_peer_token(fiber::quic::QuicConnection &conn, std::uint64_t sequence, const std::uint8_t *token) {
    fiber::quic::QuicNewConnectionIdFrame frame{};
    frame.sequence_number = sequence;
    frame.retire_prior_to = 0;
    frame.cid_len = 8;
    const std::uint8_t cid_bytes[8] = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28};
    std::memcpy(frame.cid, cid_bytes, 8);
    std::memcpy(frame.stateless_reset_token, token, fiber::quic::kStatelessResetTokenLength);
    auto received = conn.recv_new_connection_id_frame(frame);
    ASSERT_TRUE(received.has_value()) << static_cast<int>(received.error());
}

// Build a short-header datagram (48 bytes) whose trailing 16 bytes equal the
// supplied token. The fixed bit is set and the form bit clear so the parser
// accepts it as a 1-RTT short header; the body is garbage so decryption fails.
void fill_reset_datagram(std::array<std::uint8_t, 48> &out, const std::uint8_t *tail_token) {
    out.fill(0x11);
    out[0] = static_cast<std::uint8_t>(fiber::quic::kPacketFlagFixed) | 0x03u; // short header, pn_len field = 4
    for (std::size_t i = 1; i <= 8; ++i) {
        out[i] = static_cast<std::uint8_t>(0x30U + i); // 8-byte DCID (value unused on the decrypt-failure path)
    }
    std::memcpy(out.data() + out.size() - fiber::quic::kStatelessResetTokenLength, tail_token,
                fiber::quic::kStatelessResetTokenLength);
}

} // namespace

// Direct unit test of the detection predicate: matches the peer token, skips the
// initial (sequence-0) CID's zero token, enforces the minimum-length guard, and
// rejects non-matching tails — all in constant time.
TEST(QuicStatelessResetTest, DetectsStatelessResetMatchesPeerToken) {
    ShutdownCallbackState state{};
    fiber::quic::QuicConnection conn(reset_test_options(state));

    const std::uint8_t token[fiber::quic::kStatelessResetTokenLength] = {
            0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};
    store_peer_token(conn, 1, token);

    // Trailing 16 bytes equal the sequence-1 token → detected.
    std::array<std::uint8_t, 40> match{};
    std::memcpy(match.data() + match.size() - fiber::quic::kStatelessResetTokenLength, token,
                fiber::quic::kStatelessResetTokenLength);
    EXPECT_TRUE(conn.detects_stateless_reset(match.data(), match.size()));

    // Trailing 16 bytes are all zero. The sequence-0 slot also holds a zero
    // token, but the initial CID is skipped (no token is ever carried for it),
    // so this must NOT match.
    std::array<std::uint8_t, 40> zeros{};
    EXPECT_FALSE(conn.detects_stateless_reset(zeros.data(), zeros.size()));

    // Trailing 16 bytes match no stored token → not detected.
    std::array<std::uint8_t, 40> other{};
    for (std::size_t i = 0; i < other.size(); ++i) {
        other[i] = static_cast<std::uint8_t>(0xf0U + (i & 0x0fU));
    }
    EXPECT_FALSE(conn.detects_stateless_reset(other.data(), other.size()));

    // Packet no longer than the token itself → not detected (min-length guard).
    std::array<std::uint8_t, 16> tiny{};
    EXPECT_FALSE(conn.detects_stateless_reset(tiny.data(), tiny.size()));
    EXPECT_FALSE(conn.detects_stateless_reset(nullptr, 64));
}

// A short-header datagram that fails to decrypt, whose final 16 bytes equal a
// peer-advertised stateless_reset_token, MUST silently enter the DRAINING state
// with no CONNECTION_CLOSE frame queued (RFC 9000 §10.2.2 / §10.3).
TEST(QuicStatelessResetTest, UndecryptableShortHeaderWithMatchingTokenDrainsSilently) {
    ShutdownCallbackState state{};
    fiber::quic::QuicConnection conn(reset_test_options(state));
    mark_established_with_app_keys(conn);
    install_application_read_keys(conn);

    const std::uint8_t token[fiber::quic::kStatelessResetTokenLength] = {
            0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};
    store_peer_token(conn, 1, token);

    std::array<std::uint8_t, 48> datagram{};
    fill_reset_datagram(datagram, token);

    std::array<std::uint8_t, 1400> plaintext{};
    fiber::quic::QuicReceivedDatagram rd{};
    rd.data = datagram.data();
    rd.len = datagram.size();
    auto result = fiber::quic::quic_process_datagram(conn, rd, plaintext.data(), plaintext.size(),
                                                     /*short_dcid_len=*/8);

    // The reset is detected and swallowed (not dropped as a decrypt error).
    EXPECT_TRUE(result.has_value()) << "expected reset detection, got error "
                                    << (result.has_value() ? 0 : static_cast<int>(result.error()));
    EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Draining);
    EXPECT_EQ(conn.close_source(), fiber::quic::QuicCloseSource::StatelessReset);
    EXPECT_TRUE(conn.terminal_closing());
    // Silent drain: no CONNECTION_CLOSE frame on any encryption level.
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ConnectionClose),
              0U);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Application,
                                       fiber::quic::QuicFrameType::ConnectionCloseApp),
              0U);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Initial,
                                       fiber::quic::QuicFrameType::ConnectionClose),
              0U);
    EXPECT_EQ(count_pending_frame_type(conn, fiber::quic::QuicEncryptionLevel::Handshake,
                                       fiber::quic::QuicFrameType::ConnectionClose),
              0U);
}

// A short-header datagram that fails to decrypt but whose final 16 bytes match
// NO peer token is NOT a stateless reset: it is dropped as a decrypt error and
// the connection stays open.
TEST(QuicStatelessResetTest, UndecryptableShortHeaderWithoutMatchingTokenIsDropped) {
    ShutdownCallbackState state{};
    fiber::quic::QuicConnection conn(reset_test_options(state));
    mark_established_with_app_keys(conn);
    install_application_read_keys(conn);

    const std::uint8_t real_token[fiber::quic::kStatelessResetTokenLength] = {
            0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};
    store_peer_token(conn, 1, real_token);

    const std::uint8_t bogus_token[fiber::quic::kStatelessResetTokenLength] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
    std::array<std::uint8_t, 48> datagram{};
    fill_reset_datagram(datagram, bogus_token);

    std::array<std::uint8_t, 1400> plaintext{};
    fiber::quic::QuicReceivedDatagram rd{};
    rd.data = datagram.data();
    rd.len = datagram.size();
    auto result = fiber::quic::quic_process_datagram(conn, rd, plaintext.data(), plaintext.size(), 8);

    EXPECT_FALSE(result.has_value());
    EXPECT_NE(conn.state(), fiber::quic::QuicConnectionState::Draining);
    EXPECT_FALSE(conn.terminal_closing());
    EXPECT_EQ(conn.close_source(), fiber::quic::QuicCloseSource::None);
}
