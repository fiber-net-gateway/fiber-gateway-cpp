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

#include "QuicTestLoop.h"

namespace {

using namespace std::chrono_literals;

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
    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &fiber::test::quic_loop();
    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic);

    auto result = h3.start();

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Running);
    EXPECT_EQ(h3.role(), fiber::quic::QuicConnectionRole::Server);
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
    ASSERT_TRUE(h3.start().has_value());

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
    ASSERT_TRUE(h3.start().has_value());

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
    ASSERT_TRUE(h3.start().has_value());

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
    ASSERT_TRUE(h3.start().has_value());

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
    ASSERT_TRUE(h3.start().has_value());

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

TEST(Http3ConnectionTest, RejectsSecondQpackEncoderStream) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic);
    ASSERT_TRUE(h3.start().has_value());

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
