#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <string>

#include "async/Spawn.h"
#include "async/Timeout.h"
#include "common/IoError.h"
#include "common/mem/BufPool.h"
#include "event/EventLoopGroup.h"
#include "http/ClientHttp1Exchange.h"
#include "http/Http1ClientConnection.h"
#include "net/TcpListener.h"
#include "net/TcpStream.h"

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;

struct CaptureOutcome {
    fiber::common::IoErr err = fiber::common::IoErr::Unknown;
    std::string bytes;
};

fiber::common::IoResult<std::uint16_t> resolve_port(int fd) {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress local;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), len, local)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return local.port();
}

DetachedTask run_capture_server(fiber::event::EventLoop *loop,
                                std::promise<std::uint16_t> *port_promise,
                                std::size_t expected_size,
                                std::promise<CaptureOutcome> *outcome_promise) {
    CaptureOutcome outcome;

    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions listen_options{};
    auto bind_result = listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), listen_options);
    if (!bind_result) {
        port_promise->set_value(0);
        outcome.err = bind_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    auto port_result = resolve_port(listener.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        outcome.err = port_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    auto accept_result = co_await listener.accept();
    listener.close();
    if (!accept_result) {
        outcome.err = accept_result.error();
        outcome_promise->set_value(std::move(outcome));
        co_return;
    }

    fiber::net::TcpStream stream(*loop, accept_result->release_fd(), accept_result->take_peer());
    while (outcome.bytes.size() < expected_size) {
        char chunk[256];
        const std::size_t remaining = expected_size - outcome.bytes.size();
        auto read_result = co_await fiber::async::timeout_for(
            [&]() { return stream.read(chunk, std::min<std::size_t>(remaining, sizeof(chunk))); }, 2s);
        if (!read_result) {
            outcome.err = read_result.error();
            outcome_promise->set_value(std::move(outcome));
            co_return;
        }
        if (*read_result == 0) {
            outcome.err = fiber::common::IoErr::ConnReset;
            outcome_promise->set_value(std::move(outcome));
            co_return;
        }
        outcome.bytes.append(chunk, *read_result);
    }

    stream.close();
    outcome.err = fiber::common::IoErr::None;
    outcome_promise->set_value(std::move(outcome));
}

DetachedTask run_content_length_client(fiber::event::EventLoop *loop,
                                       std::uint16_t port,
                                       std::promise<fiber::common::IoErr> *result_promise,
                                       std::promise<bool> *request_done_promise) {
    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, std::move(conn_options));
    auto connect_result = co_await connection.connect();
    if (!connect_result) {
        result_promise->set_value(connect_result.error());
        request_done_promise->set_value(false);
        co_return;
    }

    fiber::common::IoErr result = fiber::common::IoErr::Unknown;
    bool request_done = false;
    {
        fiber::mem::BufPool pool;
        fiber::http::HttpHeaders headers(pool);
        headers.add_view("host", "example.com");
        headers.add_view("x-test", "1");

        fiber::http::ClientHttp1Exchange exchange(connection);
        fiber::http::Http1RequestHead head;
        head.method = fiber::http::HttpMethod::Post;
        head.target = "/submit";
        head.headers = &headers;
        head.body_mode = fiber::http::Http1RequestBodyMode::ContentLength;
        head.content_length = 5;

        auto header_result = co_await exchange.send_header(head, false);
        if (!header_result) {
            result = header_result.error();
        } else {
            auto body_result =
                co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>("hello"), 5, true);
            if (!body_result) {
                result = body_result.error();
            } else {
                result = fiber::common::IoErr::None;
                request_done = exchange.request_complete();
            }
        }
    }

    connection.close();
    result_promise->set_value(result);
    request_done_promise->set_value(request_done);
}

DetachedTask run_chunked_client(fiber::event::EventLoop *loop,
                                std::uint16_t port,
                                std::promise<fiber::common::IoErr> *result_promise,
                                std::promise<bool> *request_done_promise) {
    fiber::http::Http1ClientConnectionOptions conn_options;
    conn_options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, std::move(conn_options));
    auto connect_result = co_await connection.connect();
    if (!connect_result) {
        result_promise->set_value(connect_result.error());
        request_done_promise->set_value(false);
        co_return;
    }

    fiber::common::IoErr result = fiber::common::IoErr::Unknown;
    bool request_done = false;
    {
        fiber::mem::BufPool pool;
        fiber::http::HttpHeaders headers(pool);
        headers.add_view("host", "example.com");
        headers.add_view("x-test", "1");
        fiber::http::HttpHeaders trailers(pool);
        trailers.add_view("x-checksum", "123");

        fiber::http::ClientHttp1Exchange exchange(connection);
        fiber::http::Http1RequestHead head;
        head.method = fiber::http::HttpMethod::Post;
        head.target = "/upload";
        head.headers = &headers;
        head.body_mode = fiber::http::Http1RequestBodyMode::Chunked;

        auto header_result = co_await exchange.send_header(head, false);
        if (!header_result) {
            result = header_result.error();
        } else {
            auto body_result =
                co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>("hello"), 5, false);
            if (!body_result) {
                result = body_result.error();
            } else {
                auto trailer_result = co_await exchange.send_trailer(trailers);
                if (!trailer_result) {
                    result = trailer_result.error();
                } else {
                    result = fiber::common::IoErr::None;
                    request_done = exchange.request_complete();
                }
            }
        }
    }

    connection.close();
    result_promise->set_value(result);
    request_done_promise->set_value(request_done);
}

TEST(ClientHttp1ExchangeTest, SendHeaderAndContentLengthBodyWriteRawHttp1Request) {
    const std::string expected =
        "POST /submit HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "host: example.com\r\n"
        "x-test: 1\r\n"
        "\r\n"
        "hello";

    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<CaptureOutcome> capture_promise;
    auto capture_future = capture_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_capture_server(&group.at(0), &port_promise, expected.size(), &capture_promise);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<fiber::common::IoErr> result_promise;
    auto result_future = result_promise.get_future();
    std::promise<bool> request_done_promise;
    auto request_done_future = request_done_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_content_length_client(&group.at(0), port, &result_promise, &request_done_promise);
    });

    EXPECT_EQ(result_future.get(), fiber::common::IoErr::None);
    EXPECT_TRUE(request_done_future.get());

    CaptureOutcome capture = capture_future.get();
    EXPECT_EQ(capture.err, fiber::common::IoErr::None);
    EXPECT_EQ(capture.bytes, expected);

    group.stop();
    group.join();
}

TEST(ClientHttp1ExchangeTest, SendChunkedBodyAndTrailerWriteRawHttp1Request) {
    const std::string expected =
        "POST /upload HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "host: example.com\r\n"
        "x-test: 1\r\n"
        "\r\n"
        "5\r\n"
        "hello\r\n"
        "0\r\n"
        "x-checksum: 123\r\n"
        "\r\n";

    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::promise<CaptureOutcome> capture_promise;
    auto capture_future = capture_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_capture_server(&group.at(0), &port_promise, expected.size(), &capture_promise);
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<fiber::common::IoErr> result_promise;
    auto result_future = result_promise.get_future();
    std::promise<bool> request_done_promise;
    auto request_done_future = request_done_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_chunked_client(&group.at(0), port, &result_promise, &request_done_promise);
    });

    EXPECT_EQ(result_future.get(), fiber::common::IoErr::None);
    EXPECT_TRUE(request_done_future.get());

    CaptureOutcome capture = capture_future.get();
    EXPECT_EQ(capture.err, fiber::common::IoErr::None);
    EXPECT_EQ(capture.bytes, expected);

    group.stop();
    group.join();
}

} // namespace
