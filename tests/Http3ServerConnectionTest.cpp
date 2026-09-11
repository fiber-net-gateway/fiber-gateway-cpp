#include "Http3ConnectionTestSupport.h"

namespace {
Http3RequestRunResult run_http3_request_headers(const HeaderList &headers, bool expect_handler,
                                                bool enable_extended_connect = false) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    ServerRequestContext ctx;
    auto snapshot_promise = std::make_shared<std::promise<CapturedHttp3Request>>();
    auto snapshot_future = snapshot_promise->get_future();
    ctx.handler = [snapshot_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        snapshot_promise->set_value(capture_request(exchange));
        co_return;
    };

    ctx.options.settings.enable_connect_protocol = enable_extended_connect;

    ServerFixture fixture(quic_options, ctx.options, ctx.handler);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    EXPECT_TRUE(start.ok) << static_cast<int>(start.error);

    std::vector<std::uint8_t> request = headers_frame(headers);
    std::promise<void> feed_done;
    auto feed_future = feed_done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &request, &feed_done]() -> fiber::async::DetachedTask {
        return feed_request_stream_then_wait(&quic, &request, &feed_done);
    });

    if (feed_future.wait_for(2s) != std::future_status::ready) {
        ADD_FAILURE() << "HTTP/3 request feed did not complete";
    }

    Http3RequestRunResult result;
    result.handler_status = snapshot_future.wait_for(expect_handler ? 2s : 0ms);
    if (result.handler_status == std::future_status::ready) {
        result.snapshot = snapshot_future.get();
    }

    fixture.finish();
    group.stop();
    group.join();
    return result;
}

Http3BodyReadOutcome
run_http3_request_body(const std::vector<std::uint8_t> &request, bool delay_fin = false, std::size_t read_limit = 64,
                       std::size_t stream_buffer_limit = fiber::quic::kQuicDefaultStreamBufferSize) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    quic_options.recv_flow.stream_buffer_limit = stream_buffer_limit;
    ServerRequestContext ctx;
    auto outcome_promise = std::make_shared<std::promise<Http3BodyReadOutcome>>();
    auto outcome_future = outcome_promise->get_future();
    ctx.handler = [outcome_promise, read_limit](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        Http3BodyReadOutcome outcome;
        outcome.body_spec = exchange.request_body_spec();
        auto first = co_await exchange.read_body(read_limit);
        if (!first) {
            outcome.first_error = first.error();
            outcome_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.first_complete = first->complete();
        auto first_bytes = chain_to_bytes(std::move(*first));
        outcome.first_body.assign(reinterpret_cast<const char *>(first_bytes.data()), first_bytes.size());

        auto second = co_await exchange.read_body(read_limit);
        if (!second) {
            outcome.second_error = second.error();
            outcome.trailer_value = std::string(exchange.request_trailers().get("digest"));
            outcome.trailers_complete = exchange.request_trailers_complete();
            outcome_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.second_complete = second->complete();
        auto second_bytes = chain_to_bytes(std::move(*second));
        outcome.second_body.assign(reinterpret_cast<const char *>(second_bytes.data()), second_bytes.size());
        outcome.trailer_value = std::string(exchange.request_trailers().get("digest"));
        outcome.trailers_complete = exchange.request_trailers_complete();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    };


    ServerFixture fixture(quic_options, ctx.options, ctx.handler);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    EXPECT_TRUE(start.ok) << static_cast<int>(start.error);

    std::promise<void> feed_done;
    auto feed_future = feed_done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &request, &feed_done, delay_fin]() -> fiber::async::DetachedTask {
        if (delay_fin) {
            return feed_request_stream_then_delayed_fin(&quic, &request, &feed_done);
        }
        return feed_request_stream_then_wait(&quic, &request, &feed_done);
    });

    if (feed_future.wait_for(2s) != std::future_status::ready) {
        ADD_FAILURE() << "HTTP/3 request feed did not complete";
    }

    Http3BodyReadOutcome outcome;
    if (outcome_future.wait_for(2s) == std::future_status::ready) {
        outcome = outcome_future.get();
    } else {
        ADD_FAILURE() << "HTTP/3 body handler did not complete";
    }

    fixture.finish();
    group.stop();
    group.join();
    return outcome;
}


} // namespace

TEST(Http3ServerConnectionTest, StartsOverOpenQuicConnection) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    ServerFixture fixture(quic_options);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();

    auto result = start_h3_on_loop(group.at(0), quic, quic_options, h3);

    EXPECT_TRUE(result.ok) << static_cast<int>(result.error);
    EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Running);
    EXPECT_EQ(quic.role(), fiber::quic::QuicConnectionRole::Server);
    EXPECT_NE(quic.find_stream(3), nullptr);

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ServerConnectionTest, ServerResponseChannelWaitCompletesOnStopSending) {
    struct ResponseChannelOutcome {
        fiber::common::IoResult<void> wait_result = std::unexpected(fiber::common::IoErr::Invalid);
        bool initial_closed = true;
        bool final_closed = false;
    };

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    ServerRequestContext ctx;
    auto handler_started = std::make_shared<std::promise<void>>();
    auto started_future = handler_started->get_future();
    auto outcome_promise = std::make_shared<std::promise<ResponseChannelOutcome>>();
    auto outcome_future = outcome_promise->get_future();
    ctx.handler = [handler_started, outcome_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        ResponseChannelOutcome outcome;
        outcome.initial_closed = exchange.response_channel_closed();
        handler_started->set_value();
        outcome.wait_result = co_await exchange.wait_response_channel_closed();
        outcome.final_closed = exchange.response_channel_closed();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    };


    ServerFixture fixture(quic_options, ctx.options, ctx.handler);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    HeaderList headers{
            {":method", "GET"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "/response-channel-stop"},
    };
    std::vector<std::uint8_t> request = headers_frame(headers);
    std::promise<void> feed_done;
    auto feed_future = feed_done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &request, &feed_done]() -> fiber::async::DetachedTask {
        feed_stream(quic, 0, request, true);
        feed_done.set_value();
        co_return;
    });

    ASSERT_EQ(feed_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);

    auto stop_result = std::make_shared<std::promise<fiber::common::IoResult<void>>>();
    auto stop_future = stop_result->get_future();
    fiber::async::spawn(group.at(0), [&quic, stop_result]() -> fiber::async::DetachedTask {
        fiber::quic::QuicStopSendingFrame stop{};
        stop.id = 0;
        stop.error_code = 42;
        stop_result->set_value(quic.recv_stop_sending_frame(stop));
        co_return;
    });

    ASSERT_EQ(stop_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(stop_future.get().has_value());
    ASSERT_EQ(outcome_future.wait_for(2s), std::future_status::ready);
    ResponseChannelOutcome outcome = outcome_future.get();
    EXPECT_FALSE(outcome.initial_closed);
    EXPECT_TRUE(outcome.wait_result.has_value());
    EXPECT_TRUE(outcome.final_closed);

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ServerConnectionTest, ServerCanSendFinalResponseHeader) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    ServerRequestContext ctx;
    auto header_promise = std::make_shared<std::promise<fiber::common::IoResult<void>>>();
    auto header_future = header_promise->get_future();
    ctx.handler = [header_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        fiber::http::HttpHeaders headers(exchange.pool());
        if (headers.set("server", "fiber") == nullptr) {
            header_promise->set_value(std::unexpected(fiber::common::IoErr::NoMem));
            co_return;
        }
        auto result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .headers = &headers,
                .end_stream = true,
        });
        header_promise->set_value(result);
        co_return;
    };


    ServerFixture fixture(quic_options, ctx.options, ctx.handler);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    HeaderList headers{
            {":method", "GET"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "/"},
    };
    std::vector<std::uint8_t> request = headers_frame(headers);
    std::promise<void> feed_done;
    auto feed_future = feed_done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &request, &feed_done]() -> fiber::async::DetachedTask {
        return feed_request_stream_then_wait(&quic, &request, &feed_done);
    });

    ASSERT_EQ(feed_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(header_future.wait_for(2s), std::future_status::ready);
    auto result = header_future.get();
    EXPECT_TRUE(result.has_value()) << static_cast<int>(result.error());

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ServerConnectionTest, ServerFinalResponseStopsOnlyUnreadRequestReceiveDirection) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    ServerRequestContext ctx;
    auto header_promise = std::make_shared<std::promise<fiber::common::IoResult<void>>>();
    auto header_future = header_promise->get_future();
    ctx.handler = [header_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        auto result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 413,
                .headers = nullptr,
                .body = fiber::http::HttpBodySpec::ContentLength(0),
                .end_stream = true,
        });
        header_promise->set_value(result);
    };


    ServerFixture fixture(quic_options, ctx.options, ctx.handler);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    HeaderList headers{
            {":method", "POST"},  {":scheme", "https"},    {":authority", "example.com"},
            {":path", "/upload"}, {"content-length", "5"},
    };
    std::vector<std::uint8_t> request = headers_frame(headers);
    std::promise<void> feed_done;
    auto feed_future = feed_done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &request, &feed_done]() -> fiber::async::DetachedTask {
        return feed_stream_then_delay(&quic, &request, 0, &feed_done);
    });

    ASSERT_EQ(feed_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(header_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(header_future.get().has_value());

    std::promise<std::pair<bool, bool>> stream_state;
    auto stream_state_future = stream_state.get_future();
    fiber::async::spawn(group.at(0), [&quic, &stream_state]() -> fiber::async::DetachedTask {
        const fiber::quic::QuicStream *stream = quic.find_stream(0);
        stream_state.set_value(
                {stream != nullptr && stream->stop_sending(), stream != nullptr && stream->send_aborted()});
        co_return;
    });
    ASSERT_EQ(stream_state_future.wait_for(2s), std::future_status::ready);
    const auto [receive_stopped, response_aborted] = stream_state_future.get();
    EXPECT_TRUE(receive_stopped);
    EXPECT_FALSE(response_aborted);

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ServerConnectionTest, ServerCanWriteFinalResponseBody) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    ServerRequestContext ctx;
    auto outcome_promise = std::make_shared<std::promise<Http3BodyWriteOutcome>>();
    auto outcome_future = outcome_promise->get_future();
    ctx.handler = [outcome_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        Http3BodyWriteOutcome outcome;
        auto header = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .headers = nullptr,
                .body = fiber::http::HttpBodySpec::ContentLength(5),
                .end_stream = false,
        });
        if (!header) {
            outcome.header_error = header.error();
            outcome_promise->set_value(outcome);
            co_return;
        }
        outcome.header_ok = true;

        auto body = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>("hello"), 5, true);
        if (!body) {
            outcome.body_error = body.error();
            outcome_promise->set_value(outcome);
            co_return;
        }
        outcome.body_ok = true;
        outcome.written = *body;
        outcome_promise->set_value(outcome);
        co_return;
    };


    ServerFixture fixture(quic_options, ctx.options, ctx.handler);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    HeaderList headers{
            {":method", "GET"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "/"},
    };
    std::vector<std::uint8_t> request = headers_frame(headers);
    std::promise<void> feed_done;
    auto feed_future = feed_done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &request, &feed_done]() -> fiber::async::DetachedTask {
        return feed_request_stream_then_wait(&quic, &request, &feed_done);
    });

    ASSERT_EQ(feed_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(outcome_future.wait_for(2s), std::future_status::ready);
    auto outcome = outcome_future.get();
    EXPECT_TRUE(outcome.header_ok) << static_cast<int>(outcome.header_error);
    EXPECT_TRUE(outcome.body_ok) << static_cast<int>(outcome.body_error);
    EXPECT_EQ(outcome.written, 5U);

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ServerConnectionTest, ServerCanWriteFinalResponseBodyFromForeignNodePool) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    ServerRequestContext ctx;
    auto outcome_promise = std::make_shared<std::promise<Http3BodyWriteOutcome>>();
    auto outcome_future = outcome_promise->get_future();
    ctx.handler = [outcome_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        Http3BodyWriteOutcome outcome;
        auto header = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .headers = nullptr,
                .body = fiber::http::HttpBodySpec::ContentLength(5),
                .end_stream = false,
        });
        if (!header) {
            outcome.header_error = header.error();
            outcome_promise->set_value(outcome);
            co_return;
        }
        outcome.header_ok = true;

        fiber::mem::IoBufNodePool foreign_pool;
        fiber::mem::IoBufChain chunk(foreign_pool);
        fiber::mem::IoBuf body_buf = fiber::mem::IoBuf::allocate(5);
        if (!body_buf) {
            outcome.body_error = fiber::common::IoErr::NoMem;
            outcome_promise->set_value(outcome);
            co_return;
        }
        std::memcpy(body_buf.writable_data(), "hello", 5);
        body_buf.commit(5);
        if (!chunk.append(std::move(body_buf))) {
            outcome.body_error = fiber::common::IoErr::NoMem;
            outcome_promise->set_value(outcome);
            co_return;
        }
        chunk.mark_complete();

        auto body = co_await exchange.write_all(std::move(chunk));
        if (!body) {
            outcome.body_error = body.error();
            outcome_promise->set_value(outcome);
            co_return;
        }
        outcome.body_ok = true;
        outcome.written = *body;
        outcome.foreign_pool_cached_after_write = foreign_pool.cached_count();
        outcome_promise->set_value(outcome);
        co_return;
    };


    ServerFixture fixture(quic_options, ctx.options, ctx.handler);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    HeaderList headers{
            {":method", "GET"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "/"},
    };
    std::vector<std::uint8_t> request = headers_frame(headers);
    std::promise<void> feed_done;
    auto feed_future = feed_done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &request, &feed_done]() -> fiber::async::DetachedTask {
        return feed_request_stream_then_wait(&quic, &request, &feed_done);
    });

    ASSERT_EQ(feed_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(outcome_future.wait_for(2s), std::future_status::ready);
    auto outcome = outcome_future.get();
    EXPECT_TRUE(outcome.header_ok) << static_cast<int>(outcome.header_error);
    EXPECT_TRUE(outcome.body_ok) << static_cast<int>(outcome.body_error);
    EXPECT_EQ(outcome.written, 5U);
    EXPECT_EQ(outcome.foreign_pool_cached_after_write, 0U);

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ServerConnectionTest, ServerWriteReturnsWithUnsentPayloadTailAtStreamFlowLimit) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    constexpr std::size_t kBodySize = 2048;
    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    ServerRequestContext ctx;
    auto outcome_promise = std::make_shared<std::promise<Http3BodyWriteOutcome>>();
    auto outcome_future = outcome_promise->get_future();
    ctx.handler = [outcome_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        Http3BodyWriteOutcome outcome;
        auto header = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .body = fiber::http::HttpBodySpec::ContentLength(kBodySize),
                .end_stream = false,
        });
        if (!header) {
            outcome.header_error = header.error();
            outcome_promise->set_value(outcome);
            co_return;
        }
        outcome.header_ok = true;

        fiber::mem::IoBufNodePool &pool = fiber::event::EventLoop::current().io_buf_node_pool();
        fiber::mem::IoBufChain chunk(pool);
        fiber::mem::IoBuf body = fiber::mem::IoBuf::allocate(kBodySize);
        if (!body) {
            outcome.body_error = fiber::common::IoErr::NoMem;
            outcome_promise->set_value(outcome);
            co_return;
        }
        std::memset(body.writable_data(), 'x', kBodySize);
        body.commit(kBodySize);
        if (!chunk.append(std::move(body))) {
            outcome.body_error = fiber::common::IoErr::NoMem;
            outcome_promise->set_value(outcome);
            co_return;
        }
        chunk.mark_complete();

        auto written = co_await exchange.write(chunk);
        if (!written) {
            outcome.body_error = written.error();
            outcome_promise->set_value(outcome);
            co_return;
        }
        outcome.body_ok = true;
        outcome.written = *written;
        outcome.remaining = chunk.readable_bytes();
        outcome.complete = chunk.complete();
        outcome_promise->set_value(outcome);
        co_return;
    };


    ServerFixture fixture(quic_options, ctx.options, ctx.handler);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    HeaderList headers{
            {":method", "GET"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "/partial"},
    };
    std::vector<std::uint8_t> request = headers_frame(headers);
    std::promise<void> feed_done;
    auto feed_future = feed_done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &request, &feed_done]() -> fiber::async::DetachedTask {
        return feed_request_stream_then_wait(&quic, &request, &feed_done);
    });

    ASSERT_EQ(feed_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(outcome_future.wait_for(2s), std::future_status::ready);
    auto outcome = outcome_future.get();
    EXPECT_TRUE(outcome.header_ok) << static_cast<int>(outcome.header_error);
    EXPECT_TRUE(outcome.body_ok) << static_cast<int>(outcome.body_error);
    EXPECT_GT(outcome.written, 0U);
    EXPECT_LT(outcome.written, kBodySize);
    EXPECT_EQ(outcome.remaining, kBodySize - outcome.written);
    EXPECT_TRUE(outcome.complete);

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ServerConnectionTest, ServerWriteTimeoutDuringDataHeaderAbortsStreamAndRejectsRetry) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    ServerRequestContext ctx;
    auto outcome_promise = std::make_shared<std::promise<Http3WriteTimeoutOutcome>>();
    auto outcome_future = outcome_promise->get_future();
    ctx.handler = [outcome_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        Http3WriteTimeoutOutcome outcome;
        auto header = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .body = fiber::http::HttpBodySpec::ContentLength(5),
                .end_stream = false,
        });
        if (!header) {
            outcome.header_error = header.error();
            outcome_promise->set_value(outcome);
            co_return;
        }
        outcome.header_ok = true;

        fiber::mem::IoBufChain chunk(fiber::event::EventLoop::current().io_buf_node_pool());
        fiber::mem::IoBuf body = fiber::mem::IoBuf::allocate(5);
        if (!body) {
            outcome.first_error = fiber::common::IoErr::NoMem;
            outcome_promise->set_value(outcome);
            co_return;
        }
        std::memcpy(body.writable_data(), "hello", 5);
        body.commit(5);
        if (!chunk.append(std::move(body))) {
            outcome.first_error = fiber::common::IoErr::NoMem;
            outcome_promise->set_value(outcome);
            co_return;
        }
        chunk.mark_complete();

        auto first = co_await exchange.write(chunk, 0ms);
        if (!first) {
            outcome.first_error = first.error();
        }
        outcome.remaining = chunk.readable_bytes();
        outcome.complete = chunk.complete();

        auto retry = co_await exchange.write(chunk, 0ms);
        if (!retry) {
            outcome.retry_error = retry.error();
        }
        outcome.terminal_error = exchange.response_stats().terminal_error;
        outcome.response_channel_closed = exchange.response_channel_closed();
        outcome_promise->set_value(outcome);
        co_return;
    };


    ServerFixture fixture(quic_options, ctx.options, ctx.handler);
    auto &h3 = fixture.connection();
    auto &quic = h3.quic();
    // A minimal 200 response HEADERS frame consumes five bytes. One byte of
    // stream credit remains, so the two-byte DATA header is only partly queued
    // before the zero-timeout write fails.
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3, 6);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    HeaderList headers{
            {":method", "GET"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "/write-timeout"},
    };
    std::vector<std::uint8_t> request = headers_frame(headers);
    std::promise<void> feed_done;
    auto feed_future = feed_done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &request, &feed_done]() -> fiber::async::DetachedTask {
        return feed_request_stream_then_wait(&quic, &request, &feed_done);
    });

    ASSERT_EQ(feed_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(outcome_future.wait_for(2s), std::future_status::ready);
    Http3WriteTimeoutOutcome outcome = outcome_future.get();
    EXPECT_TRUE(outcome.header_ok) << static_cast<int>(outcome.header_error);
    EXPECT_EQ(outcome.first_error, fiber::common::IoErr::TimedOut);
    EXPECT_EQ(outcome.retry_error, fiber::common::IoErr::TimedOut);
    EXPECT_EQ(outcome.terminal_error, fiber::common::IoErr::TimedOut);
    EXPECT_EQ(outcome.remaining, 5U);
    EXPECT_TRUE(outcome.complete);
    EXPECT_TRUE(outcome.response_channel_closed);

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ServerConnectionTest, ServerRequestParsesPseudoHeadersUriAndCachesHeaderRefs) {
    HeaderList headers{
            {":method", "GET"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "/alpha//beta/../gamma/%64.txt?x=1#frag"},
            {"content-type", "text/plain"},
            {"range", "bytes=0-3"},
            {"if-range", "\"abc\""},
            {"expect", "100-continue"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, true);

    ASSERT_EQ(result.handler_status, std::future_status::ready);
    EXPECT_EQ(result.snapshot.method, fiber::http::HttpMethod::Get);
    EXPECT_EQ(result.snapshot.version, fiber::http::HttpVersion::HTTP_3_0);
    EXPECT_EQ(result.snapshot.method_view, "GET");
    EXPECT_EQ(result.snapshot.scheme, "https");
    EXPECT_TRUE(result.snapshot.protocol.empty());
    EXPECT_EQ(result.snapshot.unparsed_uri, "/alpha//beta/../gamma/%64.txt?x=1#frag");
    EXPECT_EQ(result.snapshot.path, "/alpha/gamma/d.txt");
    EXPECT_EQ(result.snapshot.query, "x=1");
    EXPECT_EQ(result.snapshot.exten, "txt");
    EXPECT_EQ(result.snapshot.host, "example.com");
    EXPECT_EQ(result.snapshot.content_type, "text/plain");
    EXPECT_EQ(result.snapshot.range, "bytes=0-3");
    EXPECT_EQ(result.snapshot.if_range, "\"abc\"");
    EXPECT_EQ(result.snapshot.expect, "100-continue");
}

TEST(Http3ServerConnectionTest, ServerRequestExposesExtendedConnectProtocolWhenEnabled) {
    HeaderList headers{
            {":method", "CONNECT"}, {":scheme", "https"},       {":authority", "example.com"},
            {":path", "/chat"},     {":protocol", "websocket"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, true, true);

    ASSERT_EQ(result.handler_status, std::future_status::ready);
    EXPECT_EQ(result.snapshot.method, fiber::http::HttpMethod::Connect);
    EXPECT_EQ(result.snapshot.scheme, "https");
    EXPECT_EQ(result.snapshot.protocol, "websocket");
}

TEST(Http3ServerConnectionTest, ServerRequestRejectsExtendedConnectProtocolWhenDisabled) {
    HeaderList headers{
            {":method", "CONNECT"}, {":scheme", "https"},       {":authority", "example.com"},
            {":path", "/chat"},     {":protocol", "websocket"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, false);

    EXPECT_EQ(result.handler_status, std::future_status::timeout);
}

TEST(Http3ServerConnectionTest, ServerRequestBorrowsQpackStaticStorage) {
    HeaderList headers{
            {":method", "GET"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "/"},
            {"accept-encoding", "gzip, deflate, br"},
            {"accept-language", "zh-CN"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, true);

    ASSERT_EQ(result.handler_status, std::future_status::ready);
    EXPECT_EQ(result.snapshot.accept_encoding, "gzip, deflate, br");
    EXPECT_EQ(result.snapshot.accept_language, "zh-CN");
    EXPECT_TRUE(result.snapshot.method_uses_static_storage);
    EXPECT_TRUE(result.snapshot.accept_encoding_uses_static_storage);
    EXPECT_TRUE(result.snapshot.accept_language_name_uses_static_storage);
}

TEST(Http3ServerConnectionTest, ServerRequestAcceptsHostHeaderMatchingAuthority) {
    HeaderList headers{
            {":method", "GET"}, {":scheme", "https"},    {":authority", "example.com"},
            {":path", "/"},     {"host", "example.com"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, true);

    ASSERT_EQ(result.handler_status, std::future_status::ready);
    EXPECT_EQ(result.snapshot.host, "example.com");
    EXPECT_EQ(result.snapshot.path, "/");
    EXPECT_TRUE(result.snapshot.body_spec.is_none());
}

TEST(Http3ServerConnectionTest, ServerRequestExposesParsedContentLength) {
    HeaderList headers{
            {":method", "POST"}, {":scheme", "https"},    {":authority", "example.com"},
            {":path", "/body"},  {"content-length", "5"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, true);

    ASSERT_EQ(result.handler_status, std::future_status::ready);
    ASSERT_TRUE(result.snapshot.body_spec.is_content_length());
    EXPECT_EQ(result.snapshot.body_spec.content_length(), 5u);
}

TEST(Http3ServerConnectionTest, ServerRequestRejectsHostAuthorityMismatch) {
    HeaderList headers{
            {":method", "GET"}, {":scheme", "https"},      {":authority", "example.com"},
            {":path", "/"},     {"host", "other.example"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, false);

    EXPECT_EQ(result.handler_status, std::future_status::timeout);
}

TEST(Http3ServerConnectionTest, ServerRequestRejectsNonOriginFormPath) {
    HeaderList headers{
            {":method", "GET"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "?q=1"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, false);

    EXPECT_EQ(result.handler_status, std::future_status::timeout);
}

TEST(Http3ServerConnectionTest, ServerRequestRejectsDuplicatePseudoHeader) {
    HeaderList headers{
            {":method", "GET"}, {":scheme", "https"}, {":authority", "example.com"},
            {":path", "/one"},  {":path", "/two"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, false);

    EXPECT_EQ(result.handler_status, std::future_status::timeout);
}

TEST(Http3ServerConnectionTest, ServerRequestRejectsForbiddenConnectionHeader) {
    HeaderList headers{
            {":method", "GET"}, {":scheme", "https"},    {":authority", "example.com"},
            {":path", "/"},     {"connection", "close"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, false);

    EXPECT_EQ(result.handler_status, std::future_status::timeout);
}

TEST(Http3ServerConnectionTest, ServerRequestRejectsInvalidTeHeader) {
    HeaderList headers{
            {":method", "GET"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/"}, {"te", "gzip"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, false);

    EXPECT_EQ(result.handler_status, std::future_status::timeout);
}

TEST(Http3ServerConnectionTest, ServerReadBodyReturnsFinWithLastData) {
    HeaderList request_headers{
            {":method", "POST"}, {":scheme", "https"},    {":authority", "example.com"},
            {":path", "/body"},  {"content-length", "5"},
    };

    std::vector<std::uint8_t> request;
    append_frame(request, headers_frame(request_headers));
    append_frame(request, data_frame("hello"));

    Http3BodyReadOutcome outcome = run_http3_request_body(request);

    ASSERT_TRUE(outcome.body_spec.is_content_length());
    EXPECT_EQ(outcome.body_spec.content_length(), 5u);
    EXPECT_EQ(outcome.first_error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.first_body, "hello");
    EXPECT_TRUE(outcome.first_complete);
    EXPECT_EQ(outcome.second_error, fiber::common::IoErr::None);
    EXPECT_TRUE(outcome.second_body.empty());
    EXPECT_TRUE(outcome.second_complete);
    EXPECT_TRUE(outcome.trailers_complete);
}

TEST(Http3ServerConnectionTest, ServerReadBodyReturnsDelayedFinSeparately) {
    HeaderList request_headers{
            {":method", "POST"}, {":scheme", "https"},    {":authority", "example.com"},
            {":path", "/body"},  {"content-length", "5"},
    };

    std::vector<std::uint8_t> request;
    append_frame(request, headers_frame(request_headers));
    append_frame(request, data_frame("hello"));

    Http3BodyReadOutcome outcome = run_http3_request_body(request, true);

    ASSERT_TRUE(outcome.body_spec.is_content_length());
    EXPECT_EQ(outcome.body_spec.content_length(), 5u);
    EXPECT_EQ(outcome.first_error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.first_body, "hello");
    EXPECT_FALSE(outcome.first_complete);
    EXPECT_EQ(outcome.second_error, fiber::common::IoErr::None);
    EXPECT_TRUE(outcome.second_body.empty());
    EXPECT_TRUE(outcome.second_complete);
    EXPECT_TRUE(outcome.trailers_complete);
}

TEST(Http3ServerConnectionTest, ServerReadBodyReturnsFinWithFullProxyChunk) {
    HeaderList request_headers{
            {":method", "POST"}, {":scheme", "https"},        {":authority", "example.com"},
            {":path", "/body"},  {"content-length", "65536"},
    };
    const std::string body(64 * 1024, 'x');

    std::vector<std::uint8_t> request;
    append_frame(request, headers_frame(request_headers));
    append_frame(request, data_frame(body));

    Http3BodyReadOutcome outcome = run_http3_request_body(request, false, 64 * 1024, 128 * 1024);

    ASSERT_TRUE(outcome.body_spec.is_content_length());
    EXPECT_EQ(outcome.body_spec.content_length(), body.size());
    EXPECT_EQ(outcome.first_error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.first_body, body);
    EXPECT_TRUE(outcome.first_complete);
    EXPECT_EQ(outcome.second_error, fiber::common::IoErr::None);
    EXPECT_TRUE(outcome.second_body.empty());
    EXPECT_TRUE(outcome.second_complete);
    EXPECT_TRUE(outcome.trailers_complete);
}

TEST(Http3ServerConnectionTest, ServerReadBodyRejectsFinInsideDataPayload) {
    HeaderList request_headers{
            {":method", "POST"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "/body"},
    };

    std::vector<std::uint8_t> request = headers_frame(request_headers);
    append_varint(request, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Data));
    append_varint(request, 10);
    request.insert(request.end(), {'h', 'e', 'l', 'l', 'o'});

    Http3BodyReadOutcome outcome = run_http3_request_body(request);

    EXPECT_EQ(outcome.first_error, fiber::common::IoErr::Invalid);
    EXPECT_TRUE(outcome.first_body.empty());
    EXPECT_FALSE(outcome.first_complete);
}

TEST(Http3ServerConnectionTest, ServerReadBodyParsesTrailersBeforeComplete) {
    HeaderList request_headers{
            {":method", "POST"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "/trailers"},
    };
    HeaderList trailers{{"digest", "sha-256=xyz"}};

    std::vector<std::uint8_t> request;
    append_frame(request, headers_frame(request_headers));
    append_frame(request, data_frame("hello"));
    append_frame(request, headers_frame(trailers));

    Http3BodyReadOutcome outcome = run_http3_request_body(request);

    EXPECT_EQ(outcome.first_error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.first_body, "hello");
    EXPECT_FALSE(outcome.first_complete);
    EXPECT_EQ(outcome.second_error, fiber::common::IoErr::None);
    EXPECT_TRUE(outcome.second_body.empty());
    EXPECT_TRUE(outcome.second_complete);
    EXPECT_TRUE(outcome.trailers_complete);
    EXPECT_EQ(outcome.trailer_value, "sha-256=xyz");
}

TEST(Http3ServerConnectionTest, ServerReadBodyCompletesEmptyBodyWithTrailers) {
    HeaderList request_headers{
            {":method", "POST"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "/empty-trailers"},
    };
    HeaderList trailers{{"digest", "sha-256=empty"}};

    std::vector<std::uint8_t> request;
    append_frame(request, headers_frame(request_headers));
    append_frame(request, headers_frame(trailers));

    Http3BodyReadOutcome outcome = run_http3_request_body(request);

    EXPECT_EQ(outcome.first_error, fiber::common::IoErr::None);
    EXPECT_TRUE(outcome.first_body.empty());
    EXPECT_TRUE(outcome.first_complete);
    EXPECT_EQ(outcome.second_error, fiber::common::IoErr::None);
    EXPECT_TRUE(outcome.second_complete);
    EXPECT_TRUE(outcome.trailers_complete);
    EXPECT_EQ(outcome.trailer_value, "sha-256=empty");
}

TEST(Http3ServerConnectionTest, ServerReadBodyRejectsPseudoHeaderInTrailers) {
    HeaderList request_headers{
            {":method", "POST"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "/bad-trailer"},
    };
    HeaderList trailers{{":path", "/not-allowed"}};

    std::vector<std::uint8_t> request;
    append_frame(request, headers_frame(request_headers));
    append_frame(request, data_frame("hello"));
    append_frame(request, headers_frame(trailers));

    Http3BodyReadOutcome outcome = run_http3_request_body(request);

    EXPECT_EQ(outcome.first_error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.first_body, "hello");
    EXPECT_FALSE(outcome.first_complete);
    EXPECT_EQ(outcome.second_error, fiber::common::IoErr::Invalid);
    EXPECT_FALSE(outcome.trailers_complete);
}


namespace {
void check_startup_drain(bool block_stream_credit) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto options = fiber::test::quic_options();
    options.loop = &group.at(0);
    ServerFixture fixture(options);
    auto &h3 = fixture.connection();
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        auto params = valid_peer_transport_params(options);
        if (block_stream_credit) {
            params.initial_max_streams_uni = 0;
        } else {
            params.initial_max_stream_data_uni = 1;
        }
        EXPECT_TRUE(h3.quic().apply_peer_transport_params(params));
        EXPECT_TRUE(h3.quic().mark_established());
        h3.start();
        co_await fiber::async::sleep(1ms);
        EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Starting);
        h3.graceful_shutdown();
        co_await fiber::async::sleep(1ms);
        EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Draining);
        EXPECT_TRUE(h3.quic().accepting_new_streams());
        if (block_stream_credit) {
            EXPECT_TRUE(h3.quic().recv_max_streams_frame({.limit = 8, .bidirectional = false}));
        } else {
            EXPECT_TRUE(h3.quic().recv_max_stream_data_frame({.id = 3, .limit = 1024}));
        }
        co_await h3.wait_closed();
        EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::NoError);
        EXPECT_TRUE(h3.quic().closing());
        done.set_value();
    });
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    fixture.finish();
    group.stop();
    group.join();
}
} // namespace
TEST(Http3ServerConnectionTest, DrainWaitsForStartupStreamCredit) { check_startup_drain(true); }
TEST(Http3ServerConnectionTest, DrainWaitsForSettingsWriteToFinish) { check_startup_drain(false); }

TEST(Http3ServerConnectionTest, CloseCancelsStartupBeforeStreamCreditArrives) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto options = fiber::test::quic_options();
    options.loop = &group.at(0);
    ServerFixture fixture(options);
    auto &h3 = fixture.connection();
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        auto params = valid_peer_transport_params(options);
        params.initial_max_streams_uni = 0;
        EXPECT_TRUE(h3.quic().apply_peer_transport_params(params));
        EXPECT_TRUE(h3.quic().mark_established());
        h3.start();
        co_await fiber::async::sleep(1ms);
        h3.close(fiber::http::Http3ErrorCode::RequestCancelled);
        co_await h3.wait_closed();
        EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Closed);
        EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::RequestCancelled);
        EXPECT_EQ(h3.quic().active_stream_count(), 0U);
        done.set_value();
    });
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ServerConnectionTest, ServerRejectsPushStreamWithStreamCreationError) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto options = fiber::test::quic_options();
    options.loop = &group.at(0);
    ServerFixture fixture(options);
    auto &h3 = fixture.connection();
    ASSERT_TRUE(start_h3_on_loop(group.at(0), h3.quic(), options, h3).ok);

    auto push = uni_stream_type(fiber::http::Http3StreamType::Push);
    append_varint(push, 0);
    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        return feed_stream_then_wait_closed(&h3.quic(), &h3, &push, 2, &done);
    });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::StreamCreationError);

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ServerConnectionTest, GracefulShutdownRejectsNewRequestStreams) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto options = fiber::test::quic_options();
    options.loop = &group.at(0);
    ServerRequestContext ctx;
    auto invoked = std::make_shared<std::promise<void>>();
    auto invoked_future = invoked->get_future();
    ctx.handler = [invoked](fiber::http::HttpExchange &) -> fiber::async::Task<void> {
        invoked->set_value();
        co_return;
    };
    ServerFixture fixture(options, ctx.options, ctx.handler);
    auto &h3 = fixture.connection();

    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        // One byte of uni stream data keeps the drain task waiting in the
        // startup join, so the rejection below races nothing.
        auto params = valid_peer_transport_params(options);
        params.initial_max_stream_data_uni = 1;
        EXPECT_TRUE(h3.quic().apply_peer_transport_params(params));
        EXPECT_TRUE(h3.quic().mark_established());
        h3.start();
        co_await fiber::async::sleep(1ms);
        EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Starting);
        h3.graceful_shutdown();
        co_await fiber::async::sleep(1ms);
        EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Draining);

        HeaderList headers{
                {":method", "GET"},
                {":scheme", "https"},
                {":authority", "example.com"},
                {":path", "/"},
        };
        auto request = headers_frame(headers);
        feed_stream(h3.quic(), 4, request, true);
        co_await fiber::async::sleep(20ms);
        EXPECT_EQ(h3.active_server_request_count(), 0U);

        EXPECT_TRUE(h3.quic().recv_max_stream_data_frame({.id = 3, .limit = 1024}));
        co_await h3.wait_closed();
        EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Closed);
        EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::NoError);
        done.set_value();
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(invoked_future.wait_for(0ms), std::future_status::timeout);

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ServerConnectionTest, DrainWaitsForBlockedResponseDeliveryBeforeSettlingCount) {
    constexpr std::size_t kBodySize = 256;
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto options = fiber::test::quic_options();
    options.loop = &group.at(0);
    ServerRequestContext ctx;
    auto handler_started = std::make_shared<std::promise<void>>();
    auto written = std::make_shared<std::promise<fiber::common::IoResult<std::size_t>>>();
    auto written_future = written->get_future();
    ctx.handler = [handler_started, written,
                   kBodySize](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        handler_started->set_value();
        auto header = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .headers = nullptr,
                .body = fiber::http::HttpBodySpec::ContentLength(kBodySize),
                .end_stream = false,
        });
        if (!header) {
            written->set_value(std::unexpected(header.error()));
            co_return;
        }
        fiber::mem::IoBuf body = fiber::mem::IoBuf::allocate(kBodySize);
        if (!body) {
            written->set_value(std::unexpected(fiber::common::IoErr::NoMem));
            co_return;
        }
        std::memset(body.writable_data(), 'x', kBodySize);
        body.commit(kBodySize);
        fiber::mem::IoBufChain chunk(fiber::event::EventLoop::current().io_buf_node_pool());
        if (!chunk.append(std::move(body))) {
            written->set_value(std::unexpected(fiber::common::IoErr::NoMem));
            co_return;
        }
        chunk.mark_complete();
        written->set_value(co_await exchange.write_all(std::move(chunk)));
    };

    ServerFixture fixture(options, ctx.options, ctx.handler);
    auto &h3 = fixture.connection();

    // Stream data credit covers the response header but not the body, so
    // delivery blocks on flow control after the handler has started.
    auto start = start_h3_on_loop(group.at(0), h3.quic(), options, h3, 16);
    ASSERT_TRUE(start.ok) << static_cast<int>(start.error);

    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        HeaderList headers{
                {":method", "GET"},
                {":scheme", "https"},
                {":authority", "example.com"},
                {":path", "/blocked"},
        };
        auto request = headers_frame(headers);
        feed_stream(h3.quic(), 0, request, true);
        co_await fiber::async::sleep(20ms);
        EXPECT_EQ(h3.active_server_request_count(), 1U);
        EXPECT_EQ(written_future.wait_for(0ms), std::future_status::timeout);

        // The request FIN was fed above; the read direction has ended, yet the
        // count only settles with Request destruction after the delivery.
        h3.graceful_shutdown();
        co_await fiber::async::sleep(20ms);
        EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Draining);
        EXPECT_EQ(h3.active_server_request_count(), 1U);
        EXPECT_EQ(written_future.wait_for(0ms), std::future_status::timeout);

        EXPECT_TRUE(h3.quic().recv_max_stream_data_frame({.id = 0, .limit = 4096}));
        co_await fiber::async::sleep(20ms);
        // The write returned and the handler finished, but the test peer never
        // ACKs, so QUIC has not delivered the response and the count must not
        // settle yet: neither handler return nor write return means delivered.
        EXPECT_EQ(written_future.wait_for(0ms), std::future_status::ready);
        auto body = written_future.get();
        EXPECT_TRUE(body.has_value()) << static_cast<int>(body.error());
        EXPECT_EQ(body.value_or(0), kBodySize);
        EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Draining);
        EXPECT_EQ(h3.active_server_request_count(), 1U);

        // Retirement of the still-unacked stream only happens on teardown.
        h3.close();
        co_await h3.wait_closed();
        EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Closed);
        EXPECT_EQ(h3.close_error(), fiber::http::Http3ErrorCode::NoError);
        EXPECT_EQ(h3.active_server_request_count(), 0U);
        done.set_value();
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);

    fixture.finish();
    group.stop();
    group.join();
}

TEST(Http3ServerConnectionTest, AcceptsRequestStreamWhileStarting) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto options = fiber::test::quic_options();
    options.loop = &group.at(0);
    ServerRequestContext ctx;
    auto invoked = std::make_shared<std::promise<void>>();
    auto invoked_future = invoked->get_future();
    ctx.handler = [invoked](fiber::http::HttpExchange &) -> fiber::async::Task<void> {
        invoked->set_value();
        co_return;
    };
    ServerFixture fixture(options, ctx.options, ctx.handler);
    auto &h3 = fixture.connection();

    std::promise<void> done;
    auto future = done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        // No uni stream credit yet: startup stays in Starting while a request
        // stream arrives and must still be admitted and dispatched.
        auto params = valid_peer_transport_params(options);
        params.initial_max_streams_uni = 0;
        EXPECT_TRUE(h3.quic().apply_peer_transport_params(params));
        EXPECT_TRUE(h3.quic().mark_established());
        h3.start();
        co_await fiber::async::sleep(1ms);
        EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Starting);

        HeaderList headers{
                {":method", "GET"},
                {":scheme", "https"},
                {":authority", "example.com"},
                {":path", "/during-startup"},
        };
        auto request = headers_frame(headers);
        feed_stream(h3.quic(), 0, request, true);
        co_await fiber::async::sleep(20ms);
        EXPECT_EQ(invoked_future.wait_for(0ms), std::future_status::ready);
        EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Starting);

        EXPECT_TRUE(h3.quic().recv_max_streams_frame({.limit = 8, .bidirectional = false}));
        co_await h3.wait_started();
        EXPECT_EQ(h3.state(), fiber::http::Http3ConnectionState::Running);
        done.set_value();
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);

    fixture.finish();
    group.stop();
    group.join();
}
