#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <vector>

#include <gtest/gtest.h>

#include "async/Spawn.h"
#include "event/EventLoop.h"
#include "event/EventLoopGroup.h"
#include "http/Http3Connection.h"
#include "http/Http3Protocol.h"
#include "quic/QuicConnection.h"
#include "quic/QuicCursor.h"
#include "quic/QuicFrame.h"
#include "quic/QuicTransportCodec.h"
#include "quic/QuicTransportParamsCodec.h"

#include "QuicTestLoop.h"

namespace {

using namespace std::chrono_literals;

struct StartResult {
    bool ok = false;
    fiber::common::IoErr error = fiber::common::IoErr::None;
};

StartResult to_start_result(fiber::common::IoResult<void> result) noexcept {
    if (result) {
        return {.ok = true};
    }
    return {.ok = false, .error = result.error()};
}

fiber::quic::QuicTransportParams valid_peer_transport_params(const fiber::quic::QuicConnection::Options &options) {
    fiber::quic::QuicTransportParams params{};
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = options.remote_connection_id;
    params.max_udp_payload_size = fiber::quic::kMinInitialDatagramSize;
    params.active_connection_id_limit = 2;
    params.initial_max_data = 4096;
    params.initial_max_stream_data_uni = 1024;
    params.initial_max_streams_uni = 8;
    params.initial_max_streams_bidi = 8;
    return params;
}

fiber::async::DetachedTask start_h3(fiber::http::Http3Connection *h3, std::promise<StartResult> *done) {
    auto result = co_await h3->start();
    done->set_value(to_start_result(result));
}

StartResult start_h3_on_loop(fiber::event::EventLoop &loop, fiber::quic::QuicConnection &quic,
                             const fiber::quic::QuicConnection::Options &options, fiber::http::Http3Connection &h3) {
    auto params = valid_peer_transport_params(options);
    auto applied = quic.apply_peer_transport_params(params);
    if (!applied) {
        return {.ok = false, .error = applied.error()};
    }
    auto established = quic.mark_established();
    if (!established) {
        return {.ok = false, .error = established.error()};
    }

    std::promise<StartResult> done;
    auto future = done.get_future();
    fiber::async::spawn(loop, [&h3, &done]() -> fiber::async::DetachedTask { return start_h3(&h3, &done); });
    if (future.wait_for(2s) != std::future_status::ready) {
        return {.ok = false, .error = fiber::common::IoErr::TimedOut};
    }
    return future.get();
}

void append_varint(std::vector<std::uint8_t> &out, std::uint64_t value) {
    std::array<std::uint8_t, 8> buf{};
    fiber::quic::QuicWriteCursor cursor(buf.data(), buf.size());
    ASSERT_TRUE(fiber::quic::quic_write_varint(cursor, value).has_value());
    out.insert(out.end(), buf.data(), buf.data() + cursor.offset());
}

std::vector<std::uint8_t> control_settings_stream(std::uint64_t blocked_streams = 0) {
    std::vector<std::uint8_t> out;
    append_varint(out, static_cast<std::uint64_t>(fiber::http::Http3StreamType::Control));
    append_varint(out, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Settings));

    std::vector<std::uint8_t> payload;
    append_varint(payload, static_cast<std::uint64_t>(fiber::http::Http3SettingId::QpackBlockedStreams));
    append_varint(payload, blocked_streams);
    append_varint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> uni_stream_type(fiber::http::Http3StreamType type) {
    std::vector<std::uint8_t> out;
    append_varint(out, static_cast<std::uint64_t>(type));
    return out;
}

void feed_stream(fiber::quic::QuicConnection &conn, std::uint64_t stream_id, const std::vector<std::uint8_t> &data,
                 bool fin = false) {
    fiber::quic::QuicStreamFrame frame{};
    frame.stream_id = stream_id;
    frame.length = data.size();
    frame.fin = fin;
    fiber::quic::QuicSlice slice{
            .data = data.empty() ? nullptr : data.data(),
            .len = data.size(),
    };
    ASSERT_TRUE(conn.recv_stream_frame(frame, slice).has_value());
}

fiber::async::DetachedTask close_and_wait(fiber::http::Http3Connection *h3, std::promise<void> *done) {
    h3->close();
    co_await h3->wait_closed();
    done->set_value();
}

fiber::async::DetachedTask feed_one_stream_then_close(fiber::quic::QuicConnection *conn,
                                                      fiber::http::Http3Connection *h3,
                                                      const std::vector<std::uint8_t> *data, std::uint64_t stream_id,
                                                      bool fin, std::promise<void> *done) {
    feed_stream(*conn, stream_id, *data, fin);
    fiber::async::spawn(fiber::event::EventLoop::current(),
                        [h3, done]() -> fiber::async::DetachedTask { return close_and_wait(h3, done); });
    co_return;
}

fiber::async::DetachedTask feed_two_streams_then_wait(fiber::quic::QuicConnection *conn,
                                                      fiber::http::Http3Connection *h3,
                                                      const std::vector<std::uint8_t> *first,
                                                      const std::vector<std::uint8_t> *second,
                                                      std::promise<void> *done) {
    feed_stream(*conn, 2, *first);
    feed_stream(*conn, 6, *second);
    fiber::async::spawn(fiber::event::EventLoop::current(), [h3, done]() -> fiber::async::DetachedTask {
        co_await h3->wait_closed();
        done->set_value();
    });
    co_return;
}

} // namespace

TEST(Http3ConnectionTest, StartsOverOpenQuicConnection) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic);

    auto result = start_h3_on_loop(group.at(0), quic, quic_options, h3);

    EXPECT_TRUE(result.ok) << static_cast<int>(result.error);
    EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Running);
    EXPECT_EQ(h3.role(), fiber::quic::QuicConnectionRole::Server);
    EXPECT_NE(quic.find_stream(3), nullptr);

    group.stop();
    group.join();
}

TEST(Http3ConnectionTest, AppliesPeerSettingsOnce) {
    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &fiber::test::quic_loop();
    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic);
    fiber::http::Http3Settings settings{};
    settings.qpack_blocked_streams = 8;

    auto first = h3.apply_peer_settings(settings);
    auto second = h3.apply_peer_settings(settings);

    EXPECT_TRUE(first.has_value());
    EXPECT_FALSE(second.has_value());
    EXPECT_TRUE(h3.peer_settings_received());
    EXPECT_EQ(h3.peer_settings().qpack_blocked_streams, 8U);
}

TEST(Http3ConnectionTest, ReadsPeerControlSettingsStream) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic);
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    auto bytes = control_settings_stream(8);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &bytes, &done]() -> fiber::async::DetachedTask {
        return feed_one_stream_then_close(&quic, &h3, &bytes, 2, false, &done);
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(h3.peer_control_stream_seen());
    EXPECT_TRUE(h3.peer_settings_received());
    EXPECT_EQ(h3.peer_settings().qpack_blocked_streams, 8U);
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::NoError);

    group.stop();
    group.join();
}

TEST(Http3ConnectionTest, RejectsSecondControlStream) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic);
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    auto first = control_settings_stream(0);
    auto second = control_settings_stream(0);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &first, &second, &done]() -> fiber::async::DetachedTask {
        return feed_two_streams_then_wait(&quic, &h3, &first, &second, &done);
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::StreamCreationError);

    group.stop();
    group.join();
}

TEST(Http3ConnectionTest, ClosingControlStreamIsCriticalStreamError) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic);
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    auto control = uni_stream_type(fiber::http::Http3StreamType::Control);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &control, &done]() -> fiber::async::DetachedTask {
        feed_stream(quic, 2, control, true);
        fiber::async::spawn(fiber::event::EventLoop::current(), [&h3, &done]() -> fiber::async::DetachedTask {
            co_await h3.wait_closed();
            done.set_value();
        });
        co_return;
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::ClosedCriticalStream);

    group.stop();
    group.join();
}

TEST(Http3ConnectionTest, ReadsQpackEncoderStreamCapacityZero) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic);
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    auto qpack = uni_stream_type(fiber::http::Http3StreamType::QpackEncoder);
    qpack.push_back(0x20); // Set Dynamic Table Capacity = 0.
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &qpack, &done]() -> fiber::async::DetachedTask {
        return feed_one_stream_then_close(&quic, &h3, &qpack, 2, false, &done);
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(h3.peer_qpack_encoder_stream_seen());
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::NoError);

    group.stop();
    group.join();
}

TEST(Http3ConnectionTest, ReadsQpackDecoderStreamUntilShutdown) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic);
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    auto qpack = uni_stream_type(fiber::http::Http3StreamType::QpackDecoder);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &qpack, &done]() -> fiber::async::DetachedTask {
        return feed_one_stream_then_close(&quic, &h3, &qpack, 2, false, &done);
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(h3.peer_qpack_decoder_stream_seen());
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::NoError);

    group.stop();
    group.join();
}

TEST(Http3ConnectionTest, ReadsQpackDecoderStreamCancellationUntilShutdown) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic);
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    auto qpack = uni_stream_type(fiber::http::Http3StreamType::QpackDecoder);
    qpack.push_back(0x40); // Stream Cancellation for stream 0.
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &qpack, &done]() -> fiber::async::DetachedTask {
        return feed_one_stream_then_close(&quic, &h3, &qpack, 2, false, &done);
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(h3.peer_qpack_decoder_stream_seen());
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::NoError);

    group.stop();
    group.join();
}

TEST(Http3ConnectionTest, RejectsSecondQpackEncoderStream) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic);
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    auto first = uni_stream_type(fiber::http::Http3StreamType::QpackEncoder);
    auto second = uni_stream_type(fiber::http::Http3StreamType::QpackEncoder);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &first, &second, &done]() -> fiber::async::DetachedTask {
        return feed_two_streams_then_wait(&quic, &h3, &first, &second, &done);
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::StreamCreationError);

    group.stop();
    group.join();
}
