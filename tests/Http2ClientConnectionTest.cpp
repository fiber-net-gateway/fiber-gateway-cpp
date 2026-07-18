#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "http/ClientHttp2Exchange.h"
#include "http/Http2ClientConnection.h"
#include "http/Http2HpackEncodeCatalog.h"
#include "net/TcpListener.h"

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;

const fiber::http::Http2HpackEncodeCatalog &test_http2_encode_catalog() {
    static fiber::http::Http2HpackEncodeCatalog catalog;
    static const bool initialized = [] {
        EXPECT_TRUE(catalog.init({}));
        return true;
    }();
    (void) initialized;
    return catalog;
}

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

DetachedTask run_hold_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                             std::atomic<bool> *stop_flag) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions listen_options{};
    auto bind_result =
            listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), listen_options);
    if (!bind_result) {
        port_promise->set_value(0);
        co_return;
    }

    auto port_result = resolve_port(listener.fd());
    port_promise->set_value(port_result ? *port_result : 0);

    auto accept_result = co_await listener.accept();
    if (!accept_result) {
        co_return;
    }

    while (!stop_flag->load(std::memory_order_acquire)) {
        co_await fiber::async::sleep(1ms);
    }
    listener.close();
}

DetachedTask run_client_connect_and_shutdown(fiber::event::EventLoop *loop, std::uint16_t port,
                                             std::atomic<bool> *stop_flag,
                                             std::promise<fiber::common::IoErr> *result_promise,
                                             std::promise<bool> *opened_promise, std::promise<bool> *no_delay_promise) {
    fiber::http::Http2ClientConnection::Options options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.tls.enabled = false;
    options.h2.outbound_hpack_catalog = &test_http2_encode_catalog();

    fiber::http::Http2ClientConnection connection(*loop, std::move(options));
    auto connect_result = co_await connection.connect(5s);
    if (!connect_result) {
        opened_promise->set_value(false);
        no_delay_promise->set_value(false);
        result_promise->set_value(connect_result.error());
        co_return;
    }

    int no_delay = 0;
    socklen_t no_delay_len = sizeof(no_delay);
    no_delay_promise->set_value(::getsockopt(connection.http2().transport().fd(), IPPROTO_TCP, TCP_NODELAY, &no_delay,
                                             &no_delay_len) == 0 &&
                                no_delay == 1);

    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange = connection.open_exchange(pool);
    auto send_result = co_await exchange.send_request_header(
            {
                    .method = fiber::http::HttpMethod::Get,
                    .scheme = "http",
                    .authority = "127.0.0.1",
                    .path = "/",
            },
            true);
    bool opened = send_result.has_value() && exchange.stream_id() != 0;
    opened_promise->set_value(opened);
    stop_flag->store(true, std::memory_order_release);

    auto run_result = co_await connection.run();
    result_promise->set_value(run_result ? fiber::common::IoErr::None : run_result.error());
}

TEST(Http2ClientConnectionTest, ConnectAllowsOpeningLocalStreamBeforeRun) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::atomic<bool> stop_flag{false};
    fiber::async::spawn(group.at(0), [&]() { return run_hold_server(&group.at(0), &port_promise, &stop_flag); });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<fiber::common::IoErr> result_promise;
    std::promise<bool> opened_promise;
    std::promise<bool> no_delay_promise;
    auto result_future = result_promise.get_future();
    auto opened_future = opened_promise.get_future();
    auto no_delay_future = no_delay_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_client_connect_and_shutdown(&group.at(0), port, &stop_flag, &result_promise, &opened_promise,
                                               &no_delay_promise);
    });

    EXPECT_TRUE(opened_future.get());
    EXPECT_TRUE(no_delay_future.get());
    fiber::common::IoErr run_result = result_future.get();
    EXPECT_TRUE(run_result == fiber::common::IoErr::None || run_result == fiber::common::IoErr::ConnReset);

    group.stop();
    group.join();
}

} // namespace
