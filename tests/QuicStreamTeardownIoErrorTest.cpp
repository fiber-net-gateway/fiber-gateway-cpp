#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <string_view>
#include <utility>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/quic/QuicConnection.h>

#include "QuicTestLoop.h"

namespace {

struct CallbackState {};

void destroy_test_stream(void * /*owner*/, fiber::quic::QuicStream &stream) noexcept { delete &stream; }

fiber::quic::QuicConnection::Options server_options(CallbackState &state) noexcept {
    fiber::quic::QuicConnection::Options options{};
    options.role = fiber::quic::QuicConnectionRole::Server;
    options.owner = &state;
    options.ops.create_stream = [](void *owner, std::uint64_t /*stream_id*/) noexcept {
        return fiber::quic::QuicStream::Lease::adopt(new (std::nothrow)
                                                             fiber::quic::QuicStream(owner, destroy_test_stream));
    };
    // Keep idle timeout out of the way; teardown is always explicit here.
    options.transport.max_idle_timeout = std::chrono::seconds(60);
    return options;
}

fiber::mem::IoBuf iobuf_of(std::string_view value) {
    fiber::mem::IoBuf buf = fiber::mem::IoBuf::allocate(value.size());
    if (buf) {
        std::memcpy(buf.writable_data(), value.data(), value.size());
        buf.commit(value.size());
    }
    return buf;
}

struct WriteResult {
    bool ok = false;
    std::size_t value = 0;
    fiber::common::IoErr error = fiber::common::IoErr::None;
};

WriteResult to_write_result(fiber::common::IoResult<std::size_t> result) {
    if (result.has_value()) {
        return {.ok = true, .value = *result};
    }
    return {.ok = false, .error = result.error()};
}

struct ReadResult {
    bool ok = false;
    std::size_t value = 0;
    fiber::common::IoErr error = fiber::common::IoErr::None;
};

ReadResult to_read_result(fiber::common::IoResult<std::size_t> result) {
    if (result.has_value()) {
        return {.ok = true, .value = *result};
    }
    return {.ok = false, .error = result.error()};
}

fiber::async::DetachedTask write_one(fiber::quic::QuicStream *stream, std::promise<WriteResult> *done) {
    auto result = co_await stream->write(iobuf_of("!"));
    done->set_value(to_write_result(result));
    fiber::event::EventLoop::current().stop();
}

fiber::async::DetachedTask read_one(fiber::quic::QuicStream *stream, std::promise<ReadResult> *done) {
    fiber::mem::IoBufChain out;
    auto result = co_await stream->read(64, out);
    done->set_value(to_read_result(result));
    fiber::event::EventLoop::current().stop();
}

fiber::async::DetachedTask close_after_delay(fiber::quic::QuicConnection *conn, std::chrono::milliseconds delay) {
    co_await fiber::async::sleep(delay);
    conn->close(fiber::quic::QuicErrorCode::NoError);
}

fiber::async::DetachedTask drain_after_delay(fiber::quic::QuicConnection *conn, std::chrono::milliseconds delay) {
    co_await fiber::async::sleep(delay);
    conn->begin_draining(fiber::quic::QuicErrorCode::InternalError);
}

struct LateIoErrors {
    fiber::common::IoErr write_err = fiber::common::IoErr::None;
    fiber::common::IoErr read_err = fiber::common::IoErr::None;
};

} // namespace

// After a local connection teardown (close + mark_closed detaches every
// stream), late stream IO must report the terminal error the teardown left on
// the stream instead of IoErr::Invalid: the write side via terminal_error_,
// the read side via the recv queue's stop state.
TEST(QuicStreamTeardownIoError, LateIoAfterLocalTeardownReportsCanceled) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        CallbackState state{};
        fiber::quic::QuicConnection conn(fiber::test::quic_endpoint(), server_options(state));
        auto stream = conn.get_or_create_peer_stream(0);
        ASSERT_TRUE(stream.has_value());
        fiber::quic::QuicStream::Lease lease{*stream};

        conn.close(fiber::quic::QuicErrorCode::NoError);
        conn.mark_closed();
        ASSERT_EQ(conn.state(), fiber::quic::QuicConnectionState::Closed);

        auto written = lease->try_write(iobuf_of("x"));
        ASSERT_FALSE(written.has_value());
        EXPECT_EQ(written.error(), fiber::common::IoErr::Canceled);

        const char raw = 'x';
        auto raw_written = lease->try_write(&raw, 1);
        ASSERT_FALSE(raw_written.has_value());
        EXPECT_EQ(raw_written.error(), fiber::common::IoErr::Canceled);

        fiber::mem::IoBufChain chain;
        auto chain_written = lease->try_write(chain);
        ASSERT_FALSE(chain_written.has_value());
        EXPECT_EQ(chain_written.error(), fiber::common::IoErr::Canceled);

        fiber::mem::IoBufChain out;
        auto read = lease->try_read(64, out);
        ASSERT_FALSE(read.has_value());
        EXPECT_EQ(read.error(), fiber::common::IoErr::Canceled);

        auto stopped = lease->stop_read(0);
        ASSERT_FALSE(stopped.has_value());
        EXPECT_EQ(stopped.error(), fiber::common::IoErr::Canceled);

        auto reset = lease->reset(0);
        ASSERT_FALSE(reset.has_value());
        EXPECT_EQ(reset.error(), fiber::common::IoErr::Canceled);
    });
}

// A writer already parked on the zero flow-control window when the connection
// closes is woken with the same terminal error the late IO path reports.
TEST(QuicStreamTeardownIoError, ParkedWriterWakesCanceledAfterLocalClose) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        CallbackState state{};
        fiber::event::EventLoopGroup group(1);
        fiber::test::QuicTestEndpoint endpoint(group.at(0));
        fiber::quic::QuicConnection conn(endpoint.get(), server_options(state));
        auto stream = conn.get_or_create_peer_stream(0);
        ASSERT_TRUE(stream.has_value());
        fiber::quic::QuicStream::Lease lease{*stream};
        // No flow-control grants: the write parks on the zero window.

        std::promise<WriteResult> done;
        auto future = done.get_future();
        group.start();
        fiber::async::spawn(group.at(0),
                            [stream = std::move(lease), &done]() { return write_one(stream.get(), &done); });
        fiber::async::spawn(group.at(0), [&conn]() { return close_after_delay(&conn, std::chrono::milliseconds(20)); });

        ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        WriteResult result = future.get();
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error, fiber::common::IoErr::Canceled);
        group.join();
    });
}

// A reader parked on an empty stream when the connection closes is woken with
// the same terminal error.
TEST(QuicStreamTeardownIoError, ParkedReaderWakesCanceledAfterLocalClose) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        CallbackState state{};
        fiber::event::EventLoopGroup group(1);
        fiber::test::QuicTestEndpoint endpoint(group.at(0));
        fiber::quic::QuicConnection conn(endpoint.get(), server_options(state));
        auto stream = conn.get_or_create_peer_stream(0);
        ASSERT_TRUE(stream.has_value());
        fiber::quic::QuicStream::Lease lease{*stream};

        std::promise<ReadResult> done;
        auto future = done.get_future();
        group.start();
        fiber::async::spawn(group.at(0),
                            [stream = std::move(lease), &done]() { return read_one(stream.get(), &done); });
        fiber::async::spawn(group.at(0), [&conn]() { return close_after_delay(&conn, std::chrono::milliseconds(20)); });

        ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        ReadResult result = future.get();
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error, fiber::common::IoErr::Canceled);
        group.join();
    });
}

// A coroutine read started after the full teardown (close + mark_closed
// detached the stream) returns the terminal error without parking. Runs
// entirely on the connection's loop to keep loop affinity.
TEST(QuicStreamTeardownIoError, LateCoroutineReadAfterTeardownReportsCanceled) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        CallbackState state{};
        fiber::event::EventLoopGroup group(1);
        fiber::test::QuicTestEndpoint endpoint(group.at(0));
        fiber::quic::QuicConnection conn(endpoint.get(), server_options(state));
        auto stream = conn.get_or_create_peer_stream(0);
        ASSERT_TRUE(stream.has_value());
        fiber::quic::QuicStream::Lease lease{*stream};

        std::promise<ReadResult> done;
        auto future = done.get_future();
        group.start();
        fiber::async::spawn(group.at(0), [&, stream = std::move(lease)]() -> fiber::async::DetachedTask {
            conn.close(fiber::quic::QuicErrorCode::NoError);
            conn.mark_closed();
            fiber::mem::IoBufChain out;
            auto result = co_await stream->read(64, out);
            done.set_value(to_read_result(result));
            fiber::event::EventLoop::current().stop();
        });

        ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        ReadResult result = future.get();
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error, fiber::common::IoErr::Canceled);
        group.join();
    });
}

// The queue-initialization guard keeps reporting Invalid for a stream that was
// never attached to a connection.
TEST(QuicStreamTeardownIoError, UninitializedStreamIoStillInvalid) {
    fiber::quic::QuicStream stream(nullptr, destroy_test_stream);

    auto written = stream.try_write(iobuf_of("x"));
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error(), fiber::common::IoErr::Invalid);

    fiber::mem::IoBufChain out;
    auto read = stream.try_read(64, out);
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error(), fiber::common::IoErr::Invalid);
}

// Remote RESET_STREAM keeps its read-side ConnReset semantics (the recv queue
// reports it directly), independent of the attached guard ordering.
TEST(QuicStreamTeardownIoError, RemoteResetStreamReadReportsConnReset) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        CallbackState state{};
        fiber::quic::QuicConnection conn(fiber::test::quic_endpoint(), server_options(state));
        auto stream = conn.get_or_create_peer_stream(0);
        ASSERT_TRUE(stream.has_value());

        fiber::quic::QuicResetStreamFrame frame{};
        frame.id = 0;
        frame.error_code = 0x100;
        frame.final_size = 0;
        ASSERT_TRUE(conn.recv_reset_stream_frame(frame).has_value());

        fiber::mem::IoBufChain out;
        auto read = (*stream)->try_read(64, out);
        ASSERT_FALSE(read.has_value());
        EXPECT_EQ(read.error(), fiber::common::IoErr::ConnReset);
    });
}

// Peer STOP_SENDING keeps its write-side BrokenPipe semantics: the send queue
// was reset, so any further write reports BrokenPipe, not the teardown
// terminal error.
TEST(QuicStreamTeardownIoError, StopSendingThenWriteReportsBrokenPipe) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        CallbackState state{};
        fiber::quic::QuicConnection conn(fiber::test::quic_endpoint(), server_options(state));
        auto stream = conn.get_or_create_peer_stream(0);
        ASSERT_TRUE(stream.has_value());

        fiber::quic::QuicStopSendingFrame frame{};
        frame.id = 0;
        frame.error_code = 0x100;
        ASSERT_TRUE(conn.recv_stop_sending_frame(frame).has_value());

        auto written = (*stream)->try_write(iobuf_of("x"));
        ASSERT_FALSE(written.has_value());
        EXPECT_EQ(written.error(), fiber::common::IoErr::BrokenPipe);
    });
}

// The shared close-source -> IoErr mapping: the single place that decides the
// teardown label for handshake waiters and stream IO alike.
TEST(QuicStreamTeardownIoError, CloseSourceIoErrorMapping) {
    EXPECT_EQ(fiber::quic::quic_close_source_io_error(fiber::quic::QuicCloseSource::None),
              fiber::common::IoErr::Canceled);
    EXPECT_EQ(fiber::quic::quic_close_source_io_error(fiber::quic::QuicCloseSource::Local),
              fiber::common::IoErr::Canceled);
    EXPECT_EQ(fiber::quic::quic_close_source_io_error(fiber::quic::QuicCloseSource::PeerConnectionClose),
              fiber::common::IoErr::ConnReset);
    EXPECT_EQ(fiber::quic::quic_close_source_io_error(fiber::quic::QuicCloseSource::StatelessReset),
              fiber::common::IoErr::ConnReset);
    EXPECT_EQ(fiber::quic::quic_close_source_io_error(fiber::quic::QuicCloseSource::IdleTimeout),
              fiber::common::IoErr::TimedOut);
}

// A peer CONNECTION_CLOSE (draining) derives ConnReset for both attached and
// detached late IO — the incident's timing window no longer changes the label.
TEST(QuicStreamTeardownIoError, PeerCloseTeardownReportsConnReset) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        CallbackState state{};
        fiber::quic::QuicConnection conn(fiber::test::quic_endpoint(), server_options(state));
        auto stream = conn.get_or_create_peer_stream(0);
        ASSERT_TRUE(stream.has_value());
        fiber::quic::QuicStream::Lease lease{*stream};

        conn.begin_draining(fiber::quic::QuicErrorCode::InternalError);
        ASSERT_EQ(conn.state(), fiber::quic::QuicConnectionState::Draining);

        auto written = lease->try_write(iobuf_of("x"));
        ASSERT_FALSE(written.has_value());
        EXPECT_EQ(written.error(), fiber::common::IoErr::ConnReset);

        fiber::mem::IoBufChain out;
        auto read = lease->try_read(64, out);
        ASSERT_FALSE(read.has_value());
        EXPECT_EQ(read.error(), fiber::common::IoErr::ConnReset);

        auto stopped = lease->stop_read(0);
        ASSERT_FALSE(stopped.has_value());
        EXPECT_EQ(stopped.error(), fiber::common::IoErr::ConnReset);

        auto reset = lease->reset(0);
        ASSERT_FALSE(reset.has_value());
        EXPECT_EQ(reset.error(), fiber::common::IoErr::ConnReset);

        // Same label after the full teardown detaches the stream.
        conn.mark_closed();
        ASSERT_EQ(conn.state(), fiber::quic::QuicConnectionState::Closed);
        written = lease->try_write(iobuf_of("x"));
        ASSERT_FALSE(written.has_value());
        EXPECT_EQ(written.error(), fiber::common::IoErr::ConnReset);
        read = lease->try_read(64, out);
        ASSERT_FALSE(read.has_value());
        EXPECT_EQ(read.error(), fiber::common::IoErr::ConnReset);
    });
}

// A writer parked on the zero window when the peer closes the connection is
// woken with ConnReset, matching what late writers report.
TEST(QuicStreamTeardownIoError, ParkedWriterWakesConnResetOnPeerClose) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        CallbackState state{};
        fiber::event::EventLoopGroup group(1);
        fiber::test::QuicTestEndpoint endpoint(group.at(0));
        fiber::quic::QuicConnection conn(endpoint.get(), server_options(state));
        auto stream = conn.get_or_create_peer_stream(0);
        ASSERT_TRUE(stream.has_value());
        fiber::quic::QuicStream::Lease lease{*stream};

        std::promise<WriteResult> done;
        auto future = done.get_future();
        group.start();
        fiber::async::spawn(group.at(0),
                            [stream = std::move(lease), &done]() { return write_one(stream.get(), &done); });
        fiber::async::spawn(group.at(0), [&conn]() { return drain_after_delay(&conn, std::chrono::milliseconds(20)); });

        ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        WriteResult result = future.get();
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error, fiber::common::IoErr::ConnReset);
        group.join();
    });
}

// A reader parked on an empty stream when the peer closes is woken with
// ConnReset: stop_receiving() forwards the derived terminal to the read
// waiter instead of a hardcoded Canceled.
TEST(QuicStreamTeardownIoError, ParkedReaderWakesConnResetOnPeerClose) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        CallbackState state{};
        fiber::event::EventLoopGroup group(1);
        fiber::test::QuicTestEndpoint endpoint(group.at(0));
        fiber::quic::QuicConnection conn(endpoint.get(), server_options(state));
        auto stream = conn.get_or_create_peer_stream(0);
        ASSERT_TRUE(stream.has_value());
        fiber::quic::QuicStream::Lease lease{*stream};

        std::promise<ReadResult> done;
        auto future = done.get_future();
        group.start();
        fiber::async::spawn(group.at(0),
                            [stream = std::move(lease), &done]() { return read_one(stream.get(), &done); });
        fiber::async::spawn(group.at(0), [&conn]() { return drain_after_delay(&conn, std::chrono::milliseconds(20)); });

        ASSERT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        ReadResult result = future.get();
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error, fiber::common::IoErr::ConnReset);
        group.join();
    });
}

// Idle timeout tears the connection down silently with TimedOut as the stream
// terminal: the parked writer and late reads/writes all report it.
TEST(QuicStreamTeardownIoError, IdleTimeoutTeardownReportsTimedOut) {
    fiber::test::run_in_quic_loop([&](fiber::mem::IoBufNodePool &) {
        CallbackState state{};
        fiber::event::EventLoopGroup group(1);
        fiber::test::QuicTestEndpoint endpoint(group.at(0));
        auto options = server_options(state);
        options.transport.max_idle_timeout = std::chrono::milliseconds(10);
        fiber::quic::QuicConnection conn(endpoint.get(), options);
        auto stream = conn.get_or_create_peer_stream(0);
        ASSERT_TRUE(stream.has_value());
        fiber::quic::QuicStream *raw = *stream;
        fiber::quic::QuicStream::Lease writer_lease{raw};
        fiber::quic::QuicStream::Lease late_lease{raw};

        std::promise<WriteResult> writer_done;
        auto writer_future = writer_done.get_future();
        std::promise<LateIoErrors> late_done;
        auto late_future = late_done.get_future();
        group.start();
        // Inline writer without the loop-stopping write_one(): the late-IO
        // orchestrator below owns stopping the loop once both results are in.
        fiber::async::spawn(group.at(0),
                            [stream = std::move(writer_lease), &writer_done]() -> fiber::async::DetachedTask {
                                auto result = co_await stream->write(iobuf_of("!"));
                                writer_done.set_value(to_write_result(result));
                            });
        fiber::async::spawn(group.at(0), [&, stream = std::move(late_lease)]() -> fiber::async::DetachedTask {
            conn.arm_idle_timer();
            co_await fiber::async::sleep(std::chrono::milliseconds(50));
            LateIoErrors errors{};
            auto written = stream->try_write(iobuf_of("x"));
            errors.write_err = written.has_value() ? fiber::common::IoErr::None : written.error();
            fiber::mem::IoBufChain out;
            auto read = stream->try_read(64, out);
            errors.read_err = read.has_value() ? fiber::common::IoErr::None : read.error();
            late_done.set_value(errors);
            fiber::event::EventLoop::current().stop();
        });

        ASSERT_EQ(writer_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        WriteResult writer_result = writer_future.get();
        EXPECT_FALSE(writer_result.ok);
        EXPECT_EQ(writer_result.error, fiber::common::IoErr::TimedOut);

        ASSERT_EQ(late_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
        LateIoErrors late_errors = late_future.get();
        EXPECT_EQ(late_errors.write_err, fiber::common::IoErr::TimedOut);
        EXPECT_EQ(late_errors.read_err, fiber::common::IoErr::TimedOut);
        EXPECT_EQ(conn.state(), fiber::quic::QuicConnectionState::Closed);
        group.join();
    });
}
