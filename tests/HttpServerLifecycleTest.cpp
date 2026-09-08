#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/ClientHttp2Exchange.h>
#include <fiber/http/Http2ClientConnection.h>
#include <fiber/http/HttpClientTlsOptions.h>
#include <fiber/http/Server.h>
#include <fiber/http/endpoint/Http2Endpoint.h>
#include <fiber/http/endpoint/Http3Endpoint.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsServerHandshakeConfig.h>

namespace {

using fiber::async::DetachedTask;
using namespace std::chrono_literals;

const char kSelfSignedCertPem[] = R"(-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUEDCdxH6aX38+fEeFx3nlY3pJwdkwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDExNzEzMDcwNVoXDTI3MDEx
NzEzMDcwNVowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEA4+tN+7EU3WmwFfjE4bn720reQJkTnAOUOYXg9zejQ75q
vHOpFxLU9z866mVpT7jVYAupmKfXrJ9U5Vd9znrWFzZt9rTdg+hISdujXjaEfEf+
GQ+66xthO2tAF3c6XokoqRpJR0GVInJoWaHBpV0PcvRb9AhRfuk+ja3W1dfdHnE8
LWutJCVK0HOWifIBGqpED3YMBNKZxFSKTCKLiqbxmnd6TT1fh8UI+AibEKhuJX4A
m3enMonO1PHeSOUY1dfXpZfdRdnYgjiyVyEw7oQL11r6O2LJZMJsoW912uIUnYrs
A4bDbMMfDgHe+PiyERCG62xydAlj1phGVlbGI/8HOQIDAQABo1MwUTAdBgNVHQ4E
FgQUvM4+Ad+L+GYd6i4nZgRFaPkRo7UwHwYDVR0jBBgwFoAUvM4+Ad+L+GYd6i4n
ZgRFaPkRo7UwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAxo8i
jbyceTsjxiMDoXd/OPtPCD2CcpWOUxMb4hdGk3pMK6xFq8c7bdMcn6oZMF7xpdHg
jDTrfa8TlPITcG/34MtvPS3hq7klCPi948Z9wbtJWGfKAl3rHYK7PIIj3wNipTcQ
IkfIlO/t6VKPSx1S9HQA6nCDOvCufOL54Mfz0vI9Y47c4O1TNtbJiiWUkP/pEjEw
RMeULfoobqmMYTjbjQ8nKC25cQAmhQ0koOqJPquPtAHvaowqBT6jDLEL+8vR4Kfc
9UqEtfRr0+7LgbcofOsseDFYMPBW2GdpPMJ2PMYsQtFMXRoomlhjdpIct6e3rRnd
GiDzEZ0VwkYlJDwF4w==
-----END CERTIFICATE-----
)";

const char kSelfSignedKeyPem[] = R"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDj6037sRTdabAV
+MThufvbSt5AmROcA5Q5heD3N6NDvmq8c6kXEtT3PzrqZWlPuNVgC6mYp9esn1Tl
V33OetYXNm32tN2D6EhJ26NeNoR8R/4ZD7rrG2E7a0AXdzpeiSipGklHQZUicmhZ
ocGlXQ9y9Fv0CFF+6T6NrdbV190ecTwta60kJUrQc5aJ8gEaqkQPdgwE0pnEVIpM
IouKpvGad3pNPV+HxQj4CJsQqG4lfgCbd6cyic7U8d5I5RjV19ell91F2diCOLJX
ITDuhAvXWvo7Yslkwmyhb3Xa4hSdiuwDhsNswx8OAd74+LIREIbrbHJ0CWPWmEZW
VsYj/wc5AgMBAAECggEAHomvmDKg1g3MHxWG46u0uCwu3T7lZrkACjkK7HTS9ke0
K23f0Qyf5kTdkvxlgN4GEOlfHuoWNrXefSAc5iaFOvT7BNw09fCQhvzbxcrOM4y9
2gPGiqvPelOjccFy26nK/eVcviRmZAgqPSA0PwDaCg/9phPbP4Lm87rAF0TmBqbq
n5s+7MXf4iFTbRIec2zTikWfbUglhNmKr3eC/4+K+hk3TX95Wltvz6dGz+godV/L
FilwLEa+e0cSTUA8FYzYtoEUiV7/8dl8VBIvQWtx8sRNNihCmnlYrJ3N8tw/hO6F
PKpfoOo+L9uRJG4LGtAkM0Pqs9U9uN5v7F5HNMxO1QKBgQD61LhiF/ftPlTRFQm2
CrnIN4PcQtIDRar/cuwgyq3F8AAfJ5PSYD/GvitaQYxa9Ya1IM3T7UPx6L3OmJl6
updR3Mh/+6BtAYwSwoWLv0tHQ01xOe9pwML52JShVocVXQFE/UXNtuffuUpXVeWk
miVen8SI4CHLeFU+6Dfcp0l3owKBgQDonbYbB9bRVzG0gbgdp2K1pxvMQizR8IkU
GsYaT/LMooBpRBOHrane+9KCztkghjmTyDKEl7jwt65fvFl0ttkipq1ISTepV6Rt
Cmdc5PnBc+ON49/6ivTGFAdU5CY3sE/7L6ngPqZq6bq8nBJ0NPcjpfEl2JfBeND8
NisrSQEjcwKBgQDlcp1QLji/LtuLf0Eo41rbCd13KTDPiXVIw6m4vW6EuGyEE0In
mZ/9f4xMvdVUh3C4U8+04z/aFFs8l18eY310hxBp8pXn4RhvOL3M/iowgCJhRuv4
wzoYLsSXaX2cTz2QDFdEPOKTRv34Mj0le1Rf4Kp5wv1nESZ5qxceo3CTHQKBgFWb
jSR/ixB57YIH53GKY6qEuJdAl2wgAOLUQ6n1WF71Qxr6gdGCGS1GMiAP7hqpK1F2
8RiZGegFQXhcQfPRQzIcc1NSFtkMtyemF4o5fq0ycEGM5qY3M4QeZOBaIrKGAblo
vjUX+XkJUb8OFUCNKZMGBCywfJEoXIklilegw3l/AoGBALtmVrX28WQ42DOYWdKD
dmDMBg1+21d8wIWs4k5bu1LdlY8XqMnV9TAHwOwGcleK2uM3AfoLOho6HwFwdyhJ
x20XBogOziImjh+cvWNpm951EC3oWHOFYPsMjX1mRCye88LQHwm3gQ8iCIOzPj+8
RB6SahiCZEhAtLq/9Q/O1bL5
-----END PRIVATE KEY-----
)";

struct BindObservation {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    int fd = -1;
    fiber::http::Server::State state = fiber::http::Server::State::Created;
};

fiber::common::IoResult<std::uint16_t> resolve_port(int fd) {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &length) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&storage), length, address)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return address.port();
}

TEST(HttpServerLifecycleTest, FailedHttp3WithoutTlsBindRollsBackListener) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<BindObservation> observation_promise;
    auto observation_future = observation_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        fiber::http::Http2Endpoint::Options options;
        fiber::http::Server server(group.at(0), {});

        fiber::http::Http2Endpoint::Options endpoint_options{};
        endpoint_options.address = {fiber::net::IpAddress::loopback_v4(), 0};
        auto *endpoint = server.add_endpoint<fiber::http::Http2Endpoint>(endpoint_options);
        EXPECT_NE(server.add_endpoint<fiber::http::Http3Endpoint>(fiber::http::Http3Endpoint::Options{}), nullptr);
        auto result = server.start();
        BindObservation observation;
        observation.error = result ? fiber::common::IoErr::None : result.error();
        observation.fd = endpoint->listener_fd();
        observation.state = server.state();
        co_await server.stop_and_wait();
        observation_promise.set_value(observation);
        co_return;
    });

    BindObservation observation = observation_future.get();
    group.stop();
    group.join();

    EXPECT_NE(observation.error, fiber::common::IoErr::None);
    EXPECT_EQ(observation.fd, -1);
    EXPECT_EQ(observation.state, fiber::http::Server::State::Created);
}

TEST(HttpServerLifecycleTest, RequestCloseCanBeIssuedOffOwnerLoop) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::http::Server server(group.at(0), {});
    std::promise<bool> bound_promise;
    auto bound_future = bound_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        fiber::http::Http2Endpoint::Options endpoint_options{};
        endpoint_options.address = {fiber::net::IpAddress::loopback_v4(), 0};
        auto *endpoint = server.add_endpoint<fiber::http::Http2Endpoint>(endpoint_options);
        auto result = server.start();
        if (result) {
            fiber::async::spawn(group.at(0), [&]() -> DetachedTask { co_await server.serve(); });
        }
        bound_promise.set_value(result.has_value());
        co_return;
    });

    EXPECT_TRUE(bound_future.get());
    server.stop();

    std::promise<fiber::http::Server::State> closed_promise;
    auto closed_future = closed_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await server.stop_and_wait();
        closed_promise.set_value(server.state());
        co_return;
    });

    const bool closed = closed_future.wait_for(5s) == std::future_status::ready;
    EXPECT_TRUE(closed);
    if (closed) {
        EXPECT_EQ(closed_future.get(), fiber::http::Server::State::Stopped);
    }
    group.stop();
    group.join();
    EXPECT_EQ(server.state(), fiber::http::Server::State::Stopped);
}

TEST(HttpServerLifecycleTest, ShutdownClosesAnIdleHttp1Connection) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<void> request_handled_promise;
    auto request_handled_future = request_handled_promise.get_future();
    fiber::http::HttpHandler handler =
            [&request_handled_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        auto result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .body = fiber::http::HttpBodySpec::None(),
                .end_stream = true,
        });
        if (result) {
            request_handled_promise.set_value();
        }
        co_return;
    };
    fiber::http::Server server(group.at(0), std::move(handler));

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        fiber::http::Http2Endpoint::Options endpoint_options{};
        endpoint_options.address = {fiber::net::IpAddress::loopback_v4(), 0};
        auto *endpoint = server.add_endpoint<fiber::http::Http2Endpoint>(endpoint_options);
        auto result = server.start();
        if (!result) {
            port_promise.set_value(0);
            co_return;
        }
        auto port = resolve_port(endpoint->listener_fd());
        port_promise.set_value(port ? *port : 0);
        fiber::async::spawn(group.at(0), [&]() -> DetachedTask { co_await server.serve(); });
        co_return;
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    const int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(client, 0);
    fiber::net::SocketAddress address(fiber::net::IpAddress::loopback_v4(), port);
    sockaddr_storage storage{};
    socklen_t length = 0;
    ASSERT_TRUE(address.to_sockaddr(storage, length));
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&storage), length), 0);
    constexpr char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));
    ASSERT_EQ(request_handled_future.wait_for(5s), std::future_status::ready);

    std::promise<void> closed_promise;
    auto closed_future = closed_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await server.stop_and_wait();
        closed_promise.set_value();
        co_return;
    });
    EXPECT_EQ(closed_future.wait_for(5s), std::future_status::ready);
    ::close(client);
    group.stop();
    group.join();
}

struct Http2LifecycleRunState {
    std::atomic_bool done{false};
    std::atomic<fiber::common::IoErr> err{fiber::common::IoErr::None};
};

fiber::async::DetachedTask watch_http2_client_close(std::shared_ptr<fiber::http::Http2ClientConnection> connection,
                                                    std::shared_ptr<Http2LifecycleRunState> state) {
    auto result = co_await connection->wait_closed();
    state->err.store(result ? fiber::common::IoErr::None : result.error(), std::memory_order_release);
    state->done.store(true, std::memory_order_release);
    co_return;
}

// Drives one request/response round trip over HTTP/2, then leaves the client
// connection open and idle. The run watcher stays attached so the test can
// observe how the connection ends.
fiber::async::DetachedTask open_idle_http2_client(fiber::event::EventLoop *loop, std::uint16_t port,
                                                  std::promise<fiber::common::IoErr> *promise) {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    auto connection = std::make_shared<fiber::http::Http2ClientConnection>(*loop);
    fiber::http::HttpClientTlsOptions tls;
    tls.server_name = "localhost";

    auto connect_result = co_await connection->connect(
            fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port), 5s, tls);
    if (!connect_result) {
        promise->set_value(connect_result.error());
        co_return;
    }
    auto run_state = std::make_shared<Http2LifecycleRunState>();
    fiber::async::spawn(*loop, [connection, run_state]() { return watch_http2_client_close(connection, run_state); });

    {
        fiber::mem::BufPool pool;
        fiber::http::ClientHttp2Exchange exchange(*connection, pool);
        auto send_result = co_await exchange.send_request_header(
                {
                        .method = fiber::http::HttpMethod::Get,
                        .scheme = "https",
                        .authority = "localhost",
                        .path = "/idle",
                },
                true);
        if (!send_result) {
            err = send_result.error();
        } else {
            auto header_result = co_await exchange.read_header();
            if (!header_result) {
                err = header_result.error();
            } else {
                auto body_result = co_await exchange.read_body(64);
                if (!body_result) {
                    err = body_result.error();
                }
            }
        }
    }

    promise->set_value(err);
    if (err != fiber::common::IoErr::None) {
        connection->shutdown();
        co_return;
    }
    // Intentionally keep the connection open and idle; the shared_ptr released
    // below still leaves the run watcher holding it until the server closes.
    co_return;
}

TEST(HttpServerLifecycleTest, ShutdownClosesAnIdleHttp2Connection) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    std::promise<void> request_handled_promise;
    auto request_handled_future = request_handled_promise.get_future();
    fiber::http::HttpHandler handler =
            [&request_handled_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        auto result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .body = fiber::http::HttpBodySpec::None(),
                .end_stream = true,
        });
        if (result) {
            request_handled_promise.set_value();
        }
        co_return;
    };

    fiber::net::TlsCredentialOptions credential_options{};
    credential_options.certificate_chain = fiber::net::TlsPemSource::from_content(kSelfSignedCertPem);
    credential_options.private_key = fiber::net::TlsPemSource::from_content(kSelfSignedKeyPem);
    auto credential = fiber::net::TlsCredential::create(credential_options);
    ASSERT_TRUE(credential);
    fiber::http::Http2Endpoint::Options server_options;
    server_options.tls.configure_callback = &fiber::net::configure_tls_with_credential;
    server_options.tls.configure_ctx = credential->get();

    fiber::http::Server server(group.at(0), std::move(handler), &group);

    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        auto endpoint_options = server_options;
        endpoint_options.address = {fiber::net::IpAddress::loopback_v4(), 0};
        auto *endpoint = server.add_endpoint<fiber::http::Http2Endpoint>(endpoint_options);
        auto result = server.start();
        if (!result) {
            port_promise.set_value(0);
            co_return;
        }
        auto port = resolve_port(endpoint->listener_fd());
        port_promise.set_value(port ? *port : 0);
        fiber::async::spawn(group.at(0), [&]() -> DetachedTask { co_await server.serve(); });
        co_return;
    });

    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<fiber::common::IoErr> client_promise;
    auto client_future = client_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return open_idle_http2_client(&group.at(0), port, &client_promise); });
    ASSERT_EQ(client_future.get(), fiber::common::IoErr::None);
    ASSERT_EQ(request_handled_future.wait_for(5s), std::future_status::ready);

    // The only thing keeping shutdown from completing is the idle HTTP/2
    // connection; if the worker walk fails to reach it this times out.
    std::promise<void> closed_promise;
    auto closed_future = closed_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await server.stop_and_wait();
        closed_promise.set_value();
        co_return;
    });
    EXPECT_EQ(closed_future.wait_for(5s), std::future_status::ready);
    group.stop();
    group.join();
}

} // namespace
