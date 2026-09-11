#include "Http3ConnectionTestSupport.h"

TEST(Http3ControlStreamsTest, AppliesPeerSettingsOnce) {
    fiber::quic::QuicConnection::Options quic_options{};
    ControlFixture h3(fiber::test::quic_endpoint(), quic_options);
    auto &quic = h3.quic();
    fiber::http::Http3Settings settings{};
    settings.qpack_blocked_streams = 8;

    auto first = h3.apply_peer_settings(settings);
    auto second = h3.apply_peer_settings(settings);

    EXPECT_TRUE(first.has_value());
    EXPECT_FALSE(second.has_value());
    EXPECT_TRUE(h3.peer_settings_received());
    EXPECT_EQ(h3.peer_settings().qpack_blocked_streams, 8U);
}

TEST(Http3ControlStreamsTest, ReadsPeerControlSettingsStream) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    ControlFixture h3(endpoint.get(), quic_options);
    auto &quic = h3.quic();
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

    h3.finish();
    group.stop();
    group.join();
}

TEST(Http3ControlStreamsTest, RejectsSecondControlStream) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    ControlFixture h3(endpoint.get(), quic_options);
    auto &quic = h3.quic();
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

    h3.finish();
    group.stop();
    group.join();
}

TEST(Http3ControlStreamsTest, ClosingControlStreamIsCriticalStreamError) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    ControlFixture h3(endpoint.get(), quic_options);
    auto &quic = h3.quic();
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

    h3.finish();
    group.stop();
    group.join();
}

TEST(Http3ControlStreamsTest, ReadsQpackEncoderStreamCapacityZero) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    ControlFixture h3(endpoint.get(), quic_options);
    auto &quic = h3.quic();
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

    h3.finish();
    group.stop();
    group.join();
}

TEST(Http3ControlStreamsTest, ReadsQpackDecoderStreamUntilShutdown) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    ControlFixture h3(endpoint.get(), quic_options);
    auto &quic = h3.quic();
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

    h3.finish();
    group.stop();
    group.join();
}

TEST(Http3ControlStreamsTest, ReadsQpackDecoderStreamCancellationUntilShutdown) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    ControlFixture h3(endpoint.get(), quic_options);
    auto &quic = h3.quic();
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

    h3.finish();
    group.stop();
    group.join();
}

TEST(Http3ControlStreamsTest, RejectsSecondQpackEncoderStream) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    ControlFixture h3(endpoint.get(), quic_options);
    auto &quic = h3.quic();
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

    h3.finish();
    group.stop();
    group.join();
}

TEST(Http3ControlStreamsTest, StopBeforeScheduledReaderStartsJoinsWithoutClosingQuic) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto options = fiber::test::quic_options();
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    ControlFixture h3(endpoint.get(), options);
    ASSERT_TRUE(start_h3_on_loop(group.at(0), h3.quic(), options, h3).ok);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        feed_stream(h3.quic(), 2, {});
        h3.stop_readers_only();
        co_await h3.wait_closed();
        EXPECT_TRUE(h3.quic().accepting_new_streams());
        done.set_value();
    });
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    h3.finish();
    group.stop();
    group.join();
}

TEST(Http3ControlStreamsTest, FirstControlFrameMustBeSettings) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    ControlFixture h3(endpoint.get(), quic_options);
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    auto control = uni_stream_type(fiber::http::Http3StreamType::Control);
    append_control_varint_frame(control, fiber::http::Http3FrameType::Goaway, 0);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &control, &done]() -> fiber::async::DetachedTask {
        return feed_stream_then_wait_closed(&quic, &h3, &control, 2, &done);
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::MissingSettings);

    h3.finish();
    group.stop();
    group.join();
}

TEST(Http3ControlStreamsTest, RejectsSecondSettingsFrame) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    ControlFixture h3(endpoint.get(), quic_options);
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    auto control = control_settings_stream(0);
    append_varint(control, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Settings));
    append_varint(control, 0);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &control, &done]() -> fiber::async::DetachedTask {
        return feed_stream_then_wait_closed(&quic, &h3, &control, 2, &done);
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::FrameUnexpected);

    h3.finish();
    group.stop();
    group.join();
}

TEST(Http3ControlStreamsTest, IgnoresUnknownUniStreamTypeSplitAcrossReads) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    ControlFixture h3(endpoint.get(), quic_options);
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &done]() -> fiber::async::DetachedTask {
        // 0x80 opens a two-byte varint that only completes with the next read;
        // the completed value 0x21 is a reserved, unknown stream type.
        std::vector<std::uint8_t> partial;
        partial.push_back(0x80);
        feed_stream(quic, 2, partial);
        std::vector<std::uint8_t> rest;
        rest.push_back(0x21);
        rest.push_back(0x42);
        feed_stream(quic, 2, rest);
        auto control = control_settings_stream(8);
        feed_stream(quic, 6, control);
        co_await fiber::async::sleep(20ms);
        EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::NoError);
        EXPECT_TRUE(h3.peer_control_stream_seen());
        EXPECT_TRUE(h3.peer_settings_received());
        EXPECT_EQ(h3.peer_settings().qpack_blocked_streams, 8U);
        done.set_value();
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);

    h3.finish();
    group.stop();
    group.join();
}

TEST(Http3ControlStreamsTest, ResetControlStreamIsClosedCriticalStreamError) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    ControlFixture h3(endpoint.get(), quic_options);
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    auto control = control_settings_stream(0);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &control, &done]() -> fiber::async::DetachedTask {
        feed_stream(quic, 2, control);
        co_await fiber::async::sleep(1ms);
        fiber::quic::QuicResetStreamFrame reset{};
        reset.id = 2;
        reset.error_code = 7;
        reset.final_size = control.size();
        EXPECT_TRUE(quic.recv_reset_stream_frame(reset).has_value());
        co_await h3.wait_closed();
        done.set_value();
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(h3.peer_control_stream_seen());
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::ClosedCriticalStream);

    h3.finish();
    group.stop();
    group.join();
}

TEST(Http3ControlStreamsTest, RejectsSecondQpackDecoderStream) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    ControlFixture h3(endpoint.get(), quic_options);
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    auto first = uni_stream_type(fiber::http::Http3StreamType::QpackDecoder);
    auto second = uni_stream_type(fiber::http::Http3StreamType::QpackDecoder);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &first, &second, &done]() -> fiber::async::DetachedTask {
        return feed_two_streams_then_wait(&quic, &h3, &first, &second, &done);
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::StreamCreationError);

    h3.finish();
    group.stop();
    group.join();
}

TEST(Http3ControlStreamsTest, AcceptedPushStreamKeepsControlStreamsAlive) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    fiber::test::QuicTestEndpoint endpoint(group.at(0));
    ControlFixture h3(endpoint.get(), quic_options, fiber::http::Http3ErrorCode::NoError);
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &h3, &done]() -> fiber::async::DetachedTask {
        auto push = uni_stream_type(fiber::http::Http3StreamType::Push);
        append_varint(push, 0);
        feed_stream(quic, 2, push);
        co_await fiber::async::sleep(1ms);
        auto control = control_settings_stream(4);
        feed_stream(quic, 6, control);
        co_await fiber::async::sleep(20ms);
        EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::NoError);
        EXPECT_TRUE(h3.peer_control_stream_seen());
        EXPECT_TRUE(h3.peer_settings_received());
        EXPECT_EQ(h3.peer_settings().qpack_blocked_streams, 4U);
        done.set_value();
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);

    h3.finish();
    group.stop();
    group.join();
}
