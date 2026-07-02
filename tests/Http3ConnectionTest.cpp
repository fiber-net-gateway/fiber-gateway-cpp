#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "common/mem/IoBufChain.h"
#include "event/EventLoop.h"
#include "event/EventLoopGroup.h"
#include "http/Http3Connection.h"
#include "http/Http3Protocol.h"
#include "http/Http3QpackEncoderIoBufWriter.h"
#include "http/HttpHeaderHash.h"
#include "http/HttpHeaders.h"
#include "http/ServerHttp3Request.h"
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

struct CapturedHttp3Request {
    fiber::http::HttpMethod method = fiber::http::HttpMethod::Unknown;
    fiber::http::HttpVersion version = fiber::http::HttpVersion::HTTP_0_9;
    std::string method_view;
    std::string unparsed_uri;
    std::string path;
    std::string query;
    std::string exten;
    std::string host;
    std::string content_type;
    std::string range;
    std::string if_range;
    std::string expect;
};

struct ServerRequestContext {
    fiber::http::HttpServerOptions options{};
    fiber::http::HttpHandler handler;
};

struct Http3RequestRunResult {
    std::future_status handler_status = std::future_status::timeout;
    CapturedHttp3Request snapshot{};
};

struct Http3BodyReadOutcome {
    fiber::common::IoErr first_error = fiber::common::IoErr::None;
    fiber::common::IoErr second_error = fiber::common::IoErr::None;
    std::string first_body;
    std::string second_body;
    std::string trailer_value;
    bool first_complete = false;
    bool second_complete = false;
    bool trailers_complete = false;
};

struct Http3BodyWriteOutcome {
    fiber::common::IoErr header_error = fiber::common::IoErr::None;
    fiber::common::IoErr body_error = fiber::common::IoErr::None;
    std::size_t written = 0;
    std::size_t foreign_pool_cached_after_write = 0;
    bool header_ok = false;
    bool body_ok = false;
};

using HeaderList = std::vector<std::pair<std::string_view, std::string_view>>;

StartResult to_start_result(fiber::common::IoResult<void> result) noexcept {
    if (result) {
        return {.ok = true};
    }
    return {.ok = false, .error = result.error()};
}

std::string field_value(const fiber::http::HttpHeaders::HeaderField *field) {
    if (field == nullptr) {
        return {};
    }
    return std::string(field->value_view());
}

CapturedHttp3Request capture_request(const fiber::http::HttpExchange &exchange) {
    return CapturedHttp3Request{
            .method = exchange.method(),
            .version = exchange.version(),
            .method_view = std::string(exchange.method_view()),
            .unparsed_uri = std::string(exchange.uri().unparsed_uri),
            .path = std::string(exchange.uri().path),
            .query = std::string(exchange.uri().query),
            .exten = std::string(exchange.uri().exten),
            .host = field_value(exchange.host_header()),
            .content_type = field_value(exchange.content_type_header()),
            .range = field_value(exchange.range_header()),
            .if_range = field_value(exchange.if_range_header()),
            .expect = field_value(exchange.expect_header()),
    };
}

fiber::quic::QuicStream::Lease create_server_request(void *owner, std::uint64_t stream_id,
                                                     fiber::http::Http3Connection &conn) noexcept {
    auto *ctx = static_cast<ServerRequestContext *>(owner);
    return fiber::http::ServerHttp3Request::create(stream_id, conn, ctx->options, ctx->handler);
}

fiber::quic::QuicTransportParams valid_peer_transport_params(const fiber::quic::QuicConnection::Options &options) {
    fiber::quic::QuicTransportParams params{};
    params.has_initial_source_connection_id = true;
    params.initial_source_connection_id = options.remote_connection_id;
    params.max_udp_payload_size = fiber::quic::kMinInitialDatagramSize;
    params.active_connection_id_limit = 2;
    params.initial_max_data = 4096;
    params.initial_max_stream_data_bidi_local = 1024;
    params.initial_max_stream_data_bidi_remote = 1024;
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

std::vector<std::uint8_t> chain_to_bytes(fiber::mem::IoBufChain chain) {
    std::vector<std::uint8_t> out;
    out.reserve(chain.readable_bytes());
    while (auto *front = chain.front()) {
        if (front->readable() == 0) {
            chain.drop_empty_front();
            continue;
        }
        const std::uint8_t *data = front->readable_data();
        out.insert(out.end(), data, data + front->readable());
        chain.consume_and_compact(front->readable());
    }
    return out;
}

std::vector<std::uint8_t> qpack_header_block(const HeaderList &headers) {
    fiber::mem::IoBufNodePool pool;
    fiber::http::Http3QpackEncoderIoBufWriter writer(
            pool, fiber::http::Http3QpackEncoder::Options{.huffman_threshold = 1024});
    for (const auto &[name, value]: headers) {
        EXPECT_EQ(writer.encode_field(name, fiber::http::http_header_name_hash(name), value),
                  fiber::common::IoErr::None);
    }

    fiber::mem::IoBufChain block(pool);
    EXPECT_EQ(writer.finish(block), fiber::common::IoErr::None);
    return chain_to_bytes(std::move(block));
}

std::vector<std::uint8_t> headers_frame(const HeaderList &headers) {
    std::vector<std::uint8_t> block = qpack_header_block(headers);
    std::vector<std::uint8_t> out;
    append_varint(out, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Headers));
    append_varint(out, block.size());
    out.insert(out.end(), block.begin(), block.end());
    return out;
}

std::vector<std::uint8_t> data_frame(std::string_view body) {
    std::vector<std::uint8_t> out;
    append_varint(out, static_cast<std::uint64_t>(fiber::http::Http3FrameType::Data));
    append_varint(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void append_frame(std::vector<std::uint8_t> &request, const std::vector<std::uint8_t> &frame) {
    request.insert(request.end(), frame.begin(), frame.end());
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

fiber::async::DetachedTask feed_request_stream_then_wait(fiber::quic::QuicConnection *conn,
                                                         const std::vector<std::uint8_t> *data,
                                                         std::promise<void> *done) {
    feed_stream(*conn, 0, *data, true);
    co_await fiber::async::sleep(20ms);
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

Http3RequestRunResult run_http3_request_headers(const HeaderList &headers, bool expect_handler) {
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

    fiber::http::Http3Connection::Options h3_options{};
    h3_options.owner = &ctx;
    h3_options.ops.create_server_request = &create_server_request;

    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic, h3_options);
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

    h3.close();
    group.stop();
    group.join();
    return result;
}

Http3BodyReadOutcome run_http3_request_body(const std::vector<std::uint8_t> &request) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::quic::QuicConnection::Options quic_options{};
    quic_options.loop = &group.at(0);
    ServerRequestContext ctx;
    auto outcome_promise = std::make_shared<std::promise<Http3BodyReadOutcome>>();
    auto outcome_future = outcome_promise->get_future();
    ctx.handler = [outcome_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        Http3BodyReadOutcome outcome;
        auto first = co_await exchange.read_body(64);
        if (!first) {
            outcome.first_error = first.error();
            outcome_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.first_complete = first->complete();
        auto first_bytes = chain_to_bytes(std::move(*first));
        outcome.first_body.assign(reinterpret_cast<const char *>(first_bytes.data()), first_bytes.size());

        auto second = co_await exchange.read_body(64);
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

    fiber::http::Http3Connection::Options h3_options{};
    h3_options.owner = &ctx;
    h3_options.ops.create_server_request = &create_server_request;

    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic, h3_options);
    auto start = start_h3_on_loop(group.at(0), quic, quic_options, h3);
    EXPECT_TRUE(start.ok) << static_cast<int>(start.error);

    std::promise<void> feed_done;
    auto feed_future = feed_done.get_future();
    fiber::async::spawn(group.at(0), [&quic, &request, &feed_done]() -> fiber::async::DetachedTask {
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

    h3.close();
    group.stop();
    group.join();
    return outcome;
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

TEST(Http3ConnectionTest, ServerCanSendFinalResponseHeader) {
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

    fiber::http::Http3Connection::Options h3_options{};
    h3_options.owner = &ctx;
    h3_options.ops.create_server_request = &create_server_request;

    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic, h3_options);
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

    h3.close();
    group.stop();
    group.join();
}

TEST(Http3ConnectionTest, ServerCanWriteFinalResponseBody) {
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

        auto body = co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>("hello"), 5, true);
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

    fiber::http::Http3Connection::Options h3_options{};
    h3_options.owner = &ctx;
    h3_options.ops.create_server_request = &create_server_request;

    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic, h3_options);
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

    h3.close();
    group.stop();
    group.join();
}

TEST(Http3ConnectionTest, ServerCanWriteFinalResponseBodyFromForeignNodePool) {
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

        auto body = co_await exchange.write_body(std::move(chunk));
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

    fiber::http::Http3Connection::Options h3_options{};
    h3_options.owner = &ctx;
    h3_options.ops.create_server_request = &create_server_request;

    fiber::quic::QuicConnection quic(quic_options);
    fiber::http::Http3Connection h3(quic, h3_options);
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

    h3.close();
    group.stop();
    group.join();
}

TEST(Http3ConnectionTest, ServerRequestParsesPseudoHeadersUriAndCachesHeaderRefs) {
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

TEST(Http3ConnectionTest, ServerRequestAcceptsHostHeaderMatchingAuthority) {
    HeaderList headers{
            {":method", "GET"}, {":scheme", "https"},    {":authority", "example.com"},
            {":path", "/"},     {"host", "example.com"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, true);

    ASSERT_EQ(result.handler_status, std::future_status::ready);
    EXPECT_EQ(result.snapshot.host, "example.com");
    EXPECT_EQ(result.snapshot.path, "/");
}

TEST(Http3ConnectionTest, ServerRequestRejectsHostAuthorityMismatch) {
    HeaderList headers{
            {":method", "GET"}, {":scheme", "https"},      {":authority", "example.com"},
            {":path", "/"},     {"host", "other.example"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, false);

    EXPECT_EQ(result.handler_status, std::future_status::timeout);
}

TEST(Http3ConnectionTest, ServerRequestRejectsNonOriginFormPath) {
    HeaderList headers{
            {":method", "GET"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "?q=1"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, false);

    EXPECT_EQ(result.handler_status, std::future_status::timeout);
}

TEST(Http3ConnectionTest, ServerRequestRejectsDuplicatePseudoHeader) {
    HeaderList headers{
            {":method", "GET"}, {":scheme", "https"}, {":authority", "example.com"},
            {":path", "/one"},  {":path", "/two"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, false);

    EXPECT_EQ(result.handler_status, std::future_status::timeout);
}

TEST(Http3ConnectionTest, ServerRequestRejectsForbiddenConnectionHeader) {
    HeaderList headers{
            {":method", "GET"}, {":scheme", "https"},    {":authority", "example.com"},
            {":path", "/"},     {"connection", "close"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, false);

    EXPECT_EQ(result.handler_status, std::future_status::timeout);
}

TEST(Http3ConnectionTest, ServerRequestRejectsInvalidTeHeader) {
    HeaderList headers{
            {":method", "GET"}, {":scheme", "https"}, {":authority", "example.com"}, {":path", "/"}, {"te", "gzip"},
    };

    Http3RequestRunResult result = run_http3_request_headers(headers, false);

    EXPECT_EQ(result.handler_status, std::future_status::timeout);
}

TEST(Http3ConnectionTest, ServerReadBodyReturnsDataBeforeCompleteForFin) {
    HeaderList request_headers{
            {":method", "POST"},
            {":scheme", "https"},
            {":authority", "example.com"},
            {":path", "/body"},
    };

    std::vector<std::uint8_t> request;
    append_frame(request, headers_frame(request_headers));
    append_frame(request, data_frame("hello"));

    Http3BodyReadOutcome outcome = run_http3_request_body(request);

    EXPECT_EQ(outcome.first_error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.first_body, "hello");
    EXPECT_FALSE(outcome.first_complete);
    EXPECT_EQ(outcome.second_error, fiber::common::IoErr::None);
    EXPECT_TRUE(outcome.second_body.empty());
    EXPECT_TRUE(outcome.second_complete);
    EXPECT_TRUE(outcome.trailers_complete);
}

TEST(Http3ConnectionTest, ServerReadBodyParsesTrailersBeforeComplete) {
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

TEST(Http3ConnectionTest, ServerReadBodyCompletesEmptyBodyWithTrailers) {
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

TEST(Http3ConnectionTest, ServerReadBodyRejectsPseudoHeaderInTrailers) {
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
