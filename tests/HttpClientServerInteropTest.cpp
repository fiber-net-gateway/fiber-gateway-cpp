#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "async/Task.h"
#include "common/IoError.h"
#include "common/mem/IoBuf.h"
#include "event/EventLoopGroup.h"
#include "http/ClientHttp1Exchange.h"
#include "http/ClientHttp2Exchange.h"
#include "http/Http1ClientConnection.h"
#include "http/Http2ClientConnection.h"
#include "http/Http2HpackEncodeCatalog.h"
#include "http/HttpServer.h"
#include "net/SocketAddress.h"

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

struct TempFile {
    std::string path;
    bool ok = false;

    TempFile(const char *tag, std::string_view data) {
        static std::atomic<std::uint32_t> next_id{1};
        path = "/tmp/fiber_http_interop_";
        path.append(tag);
        path.push_back('_');
        path.append(std::to_string(static_cast<long>(::getpid())));
        path.push_back('_');
        path.append(std::to_string(next_id.fetch_add(1, std::memory_order_relaxed)));
        path.append(".pem");

        std::ofstream out(path, std::ios::binary);
        if (!out) {
            path.clear();
            return;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        ok = out.good();
        if (!ok) {
            path.clear();
        }
    }

    ~TempFile() {
        if (!path.empty()) {
            ::unlink(path.c_str());
        }
    }
};

struct ObservedRequest {
    fiber::http::HttpMethod method = fiber::http::HttpMethod::Unknown;
    fiber::http::HttpVersion version = fiber::http::HttpVersion::HTTP_1_1;
    std::string path;
    std::string host;
    std::string body;
};

struct ClientRoundTripResult {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    int status_code = 0;
    std::string echoed_method;
    std::string echoed_path;
    std::string response_body;
    bool body_last = false;
};

struct Http2RunState {
    std::atomic_bool done{false};
    std::atomic<fiber::common::IoErr> err{fiber::common::IoErr::None};
};

const fiber::http::Http2HpackEncodeCatalog &test_http2_encode_catalog() {
    static fiber::http::Http2HpackEncodeCatalog catalog;
    static const bool initialized = [] {
        EXPECT_TRUE(catalog.init({}));
        return true;
    }();
    (void)initialized;
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

std::string chain_to_string(fiber::mem::IoBufChain chain) {
    std::string out;
    while (auto *front = chain.front()) {
        if (front->readable() == 0) {
            chain.drop_empty_front();
            continue;
        }
        out.append(reinterpret_cast<const char *>(front->readable_data()), front->readable());
        chain.consume_and_compact(front->readable());
    }
    return out;
}

template<class Exchange>
fiber::async::Task<fiber::common::IoResult<std::string>> read_body_to_string(Exchange &exchange) {
    std::string out;
    for (;;) {
        auto chunk_result = co_await exchange.read_body(64);
        if (!chunk_result) {
            co_return std::unexpected(chunk_result.error());
        }
        out.append(chain_to_string(std::move(chunk_result->data_chain)));
        if (chunk_result->last) {
            break;
        }
    }
    co_return out;
}

fiber::async::Task<fiber::common::IoResult<void>> send_final_header(
    fiber::http::HttpExchange &exchange,
    int status_code,
    const fiber::http::HttpHeaders *headers,
    fiber::http::HttpBodySpec body,
    bool end_stream) {
    co_return co_await exchange.send_header({
        .kind = fiber::http::OutgoingHeaderKind::Final,
        .status_code = status_code,
        .headers = headers,
        .body = body,
        .end_stream = end_stream,
    });
}

fiber::async::Task<fiber::common::IoResult<ObservedRequest>> collect_request(fiber::http::HttpExchange &exchange) {
    ObservedRequest observed;
    observed.method = exchange.method();
    observed.version = exchange.version();
    observed.path.assign(exchange.uri().path.data(), exchange.uri().path.size());
    std::string_view host = exchange.request_headers().get("host");
    observed.host.assign(host.data(), host.size());
    auto body_result = co_await read_body_to_string(exchange);
    if (!body_result) {
        co_return std::unexpected(body_result.error());
    }
    observed.body = std::move(*body_result);
    co_return observed;
}

fiber::async::Task<void> handle_no_body_request(fiber::http::HttpExchange &exchange,
                                                std::shared_ptr<std::promise<ObservedRequest>> observed_promise) {
    auto observed_result = co_await collect_request(exchange);
    if (!observed_result) {
        co_return;
    }
    observed_promise->set_value(*observed_result);

    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("x-echo-method", exchange.method_view());
    headers.set("x-echo-path", exchange.uri().path);
    (void)co_await send_final_header(exchange, 204, &headers, fiber::http::HttpBodySpec::None(), true);
}

fiber::async::Task<void> handle_body_request(fiber::http::HttpExchange &exchange,
                                             std::shared_ptr<std::promise<ObservedRequest>> observed_promise) {
    auto observed_result = co_await collect_request(exchange);
    if (!observed_result) {
        co_return;
    }
    observed_promise->set_value(*observed_result);

    const std::string &body = observed_result->body;
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("content-type", "text/plain");
    headers.set("x-body-size", std::to_string(body.size()));
    auto header_result = co_await send_final_header(exchange,
                                                    200,
                                                    &headers,
                                                    fiber::http::HttpBodySpec::ContentLength(body.size()),
                                                    false);
    if (!header_result) {
        co_return;
    }
    (void)co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
}

DetachedTask start_http_server(fiber::event::EventLoop *loop,
                               fiber::http::HttpHandler handler,
                               fiber::http::HttpServerOptions options,
                               fiber::event::EventLoopGroup *worker_group,
                               std::promise<std::uint16_t> *port_promise,
                               std::promise<fiber::http::HttpServer *> *server_promise) {
    auto *server = new fiber::http::HttpServer(*loop, std::move(handler), std::move(options), worker_group);
    fiber::net::ListenOptions listen_options{};
    fiber::net::SocketAddress addr(fiber::net::IpAddress::loopback_v4(), 0);
    auto bind_result = server->bind(addr, listen_options);
    if (!bind_result) {
        delete server;
        port_promise->set_value(0);
        server_promise->set_value(nullptr);
        co_return;
    }

    auto port_result = resolve_port(server->fd());
    port_promise->set_value(port_result ? *port_result : 0);
    server_promise->set_value(server);
    fiber::async::spawn(*loop, [server]() { return server->serve(); });
    co_return;
}

DetachedTask close_server_on_loop(fiber::http::HttpServer *server, std::promise<void> *done_promise) {
    if (server) {
        server->close();
    }
    done_promise->set_value();
    co_return;
}

DetachedTask run_http2_connection_task(fiber::http::Http2ClientConnection *connection, Http2RunState *state) {
    auto result = co_await connection->run();
    if (state) {
        state->err.store(result ? fiber::common::IoErr::None : result.error(), std::memory_order_release);
        state->done.store(true, std::memory_order_release);
    }
    co_return;
}

DetachedTask run_http1_client_no_body(fiber::event::EventLoop *loop,
                                      std::uint16_t port,
                                      std::promise<ClientRoundTripResult> *promise) {
    ClientRoundTripResult result;
    fiber::http::Http1ClientConnectionOptions options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, options);
    auto connect_result = co_await connection.connect();
    if (!connect_result) {
        result.err = connect_result.error();
        promise->set_value(std::move(result));
        co_return;
    }

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    headers.set("host", "localhost");

    fiber::http::ClientHttp1Exchange exchange(connection, pool);
    fiber::http::Http1RequestHead head;
    head.method = fiber::http::HttpMethod::Get;
    head.target = "/interop/http1/no-body";
    head.headers = &headers;

    auto send_result = co_await exchange.send_header(head, true);
    if (!send_result) {
        result.err = send_result.error();
        promise->set_value(std::move(result));
        co_return;
    }

    auto header_result = co_await exchange.read_header();
    if (!header_result) {
        result.err = header_result.error();
        promise->set_value(std::move(result));
        co_return;
    }

    result.status_code = (*header_result)->status_code;
    result.echoed_method = std::string((*header_result)->headers.get("x-echo-method"));
    result.echoed_path = std::string((*header_result)->headers.get("x-echo-path"));

    auto body_result = co_await exchange.read_body(64);
    if (!body_result) {
        result.err = body_result.error();
        promise->set_value(std::move(result));
        co_return;
    }
    result.response_body = chain_to_string(std::move(body_result->data_chain));
    result.body_last = body_result->last;
    promise->set_value(std::move(result));
}

DetachedTask run_http1_client_with_body(fiber::event::EventLoop *loop,
                                        std::uint16_t port,
                                        std::promise<ClientRoundTripResult> *promise) {
    ClientRoundTripResult result;
    fiber::http::Http1ClientConnectionOptions options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);

    fiber::http::Http1ClientConnection connection(*loop, options);
    auto connect_result = co_await connection.connect();
    if (!connect_result) {
        result.err = connect_result.error();
        promise->set_value(std::move(result));
        co_return;
    }

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    headers.set("host", "localhost");
    headers.set("content-type", "text/plain");

    constexpr std::string_view kBody = "http1-client-body";
    fiber::http::ClientHttp1Exchange exchange(connection, pool);
    fiber::http::Http1RequestHead head;
    head.method = fiber::http::HttpMethod::Post;
    head.target = "/interop/http1/with-body";
    head.headers = &headers;
    head.body = fiber::http::HttpBodySpec::ContentLength(kBody.size());

    auto send_result = co_await exchange.send_header(head, false);
    if (!send_result) {
        result.err = send_result.error();
        promise->set_value(std::move(result));
        co_return;
    }
    auto write_result =
        co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(kBody.data()), kBody.size(), true);
    if (!write_result) {
        result.err = write_result.error();
        promise->set_value(std::move(result));
        co_return;
    }

    auto header_result = co_await exchange.read_header();
    if (!header_result) {
        result.err = header_result.error();
        promise->set_value(std::move(result));
        co_return;
    }

    result.status_code = (*header_result)->status_code;
    auto body_result = co_await read_body_to_string(exchange);
    if (!body_result) {
        result.err = body_result.error();
        promise->set_value(std::move(result));
        co_return;
    }
    result.response_body = std::move(*body_result);
    result.body_last = true;
    promise->set_value(std::move(result));
}

DetachedTask run_http2_client_no_body(fiber::event::EventLoop *loop,
                                      std::uint16_t port,
                                      std::promise<ClientRoundTripResult> *promise) {
    ClientRoundTripResult result;
    fiber::http::Http2ClientConnection::Options options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.tls.enabled = true;
    options.tls.server_name = "localhost";
    options.h2.outbound_hpack_catalog = &test_http2_encode_catalog();

    fiber::http::Http2ClientConnection connection(*loop, std::move(options));
    auto connect_result = co_await connection.connect();
    if (!connect_result) {
        result.err = connect_result.error();
        promise->set_value(std::move(result));
        co_return;
    }

    auto run_state = std::make_shared<Http2RunState>();
    fiber::async::spawn(*loop, [conn = &connection, state = run_state.get()]() {
        return run_http2_connection_task(conn, state);
    });

    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(connection, pool);
    auto send_result = co_await exchange.send_request_header({
        .method = fiber::http::HttpMethod::Get,
        .scheme = "https",
        .authority = "localhost",
        .path = "/interop/http2/no-body",
    }, true);
    if (!send_result) {
        result.err = send_result.error();
        connection.shutdown();
    } else {
        auto header_result = co_await exchange.read_header();
        if (!header_result) {
            result.err = header_result.error();
        } else {
            result.status_code = (*header_result)->status_code;
            result.echoed_method = std::string((*header_result)->headers.get("x-echo-method"));
            result.echoed_path = std::string((*header_result)->headers.get("x-echo-path"));
            auto body_result = co_await exchange.read_body(64);
            if (!body_result) {
                result.err = body_result.error();
            } else {
                result.response_body = chain_to_string(std::move(body_result->data_chain));
                result.body_last = body_result->last;
            }
        }
        connection.shutdown();
    }

    for (int i = 0; i < 200 && !run_state->done.load(std::memory_order_acquire); ++i) {
        co_await fiber::async::sleep(1ms);
    }
    if (!run_state->done.load(std::memory_order_acquire)) {
        result.err = fiber::common::IoErr::TimedOut;
    } else {
        fiber::common::IoErr err = run_state->err.load(std::memory_order_acquire);
        if (result.err == fiber::common::IoErr::None && err != fiber::common::IoErr::None &&
            err != fiber::common::IoErr::Canceled && err != fiber::common::IoErr::ConnReset) {
            result.err = err;
        }
    }

    promise->set_value(std::move(result));
}

DetachedTask run_http2_client_with_body(fiber::event::EventLoop *loop,
                                        std::uint16_t port,
                                        std::promise<ClientRoundTripResult> *promise) {
    ClientRoundTripResult result;
    fiber::http::Http2ClientConnection::Options options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.tls.enabled = true;
    options.tls.server_name = "localhost";
    options.h2.outbound_hpack_catalog = &test_http2_encode_catalog();

    fiber::http::Http2ClientConnection connection(*loop, std::move(options));
    auto connect_result = co_await connection.connect();
    if (!connect_result) {
        result.err = connect_result.error();
        promise->set_value(std::move(result));
        co_return;
    }

    auto run_state = std::make_shared<Http2RunState>();
    fiber::async::spawn(*loop, [conn = &connection, state = run_state.get()]() {
        return run_http2_connection_task(conn, state);
    });

    constexpr std::string_view kBody = "http2-client-body";
    fiber::mem::BufPool pool;
    fiber::http::ClientHttp2Exchange exchange(connection, pool);
    auto send_result = co_await exchange.send_request_header({
        .method = fiber::http::HttpMethod::Post,
        .scheme = "https",
        .authority = "localhost",
        .path = "/interop/http2/with-body",
    }, false);
    if (!send_result) {
        result.err = send_result.error();
        connection.shutdown();
    } else {
        auto write_result =
            co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(kBody.data()), kBody.size(), true);
        if (!write_result) {
            result.err = write_result.error();
        } else {
            auto header_result = co_await exchange.read_header();
            if (!header_result) {
                result.err = header_result.error();
            } else {
                result.status_code = (*header_result)->status_code;
                auto body_result = co_await read_body_to_string(exchange);
                if (!body_result) {
                    result.err = body_result.error();
                } else {
                    result.response_body = std::move(*body_result);
                    result.body_last = true;
                }
            }
        }
        connection.shutdown();
    }

    for (int i = 0; i < 200 && !run_state->done.load(std::memory_order_acquire); ++i) {
        co_await fiber::async::sleep(1ms);
    }
    if (!run_state->done.load(std::memory_order_acquire)) {
        result.err = fiber::common::IoErr::TimedOut;
    } else {
        fiber::common::IoErr err = run_state->err.load(std::memory_order_acquire);
        if (result.err == fiber::common::IoErr::None && err != fiber::common::IoErr::None &&
            err != fiber::common::IoErr::Canceled && err != fiber::common::IoErr::ConnReset) {
            result.err = err;
        }
    }

    promise->set_value(std::move(result));
}

TEST(HttpClientServerInteropTest, Http1ClientAndServerRoundTripWithoutBody) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    auto observed_promise = std::make_shared<std::promise<ObservedRequest>>();
    auto observed_future = observed_promise->get_future();

    fiber::async::spawn(group.at(0), [&]() {
        fiber::http::HttpHandler handler = [observed_promise](fiber::http::HttpExchange &exchange) {
            return handle_no_body_request(exchange, observed_promise);
        };
        return start_http_server(&group.at(0), std::move(handler), {}, nullptr, &port_promise, &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ClientRoundTripResult> client_promise;
    auto client_future = client_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_http1_client_no_body(&group.at(0), port, &client_promise); });

    ClientRoundTripResult client = client_future.get();
    ObservedRequest observed = observed_future.get();

    EXPECT_EQ(client.err, fiber::common::IoErr::None);
    EXPECT_EQ(client.status_code, 204);
    EXPECT_EQ(client.echoed_method, "GET");
    EXPECT_EQ(client.echoed_path, "/interop/http1/no-body");
    EXPECT_TRUE(client.response_body.empty());
    EXPECT_TRUE(client.body_last);

    EXPECT_EQ(observed.method, fiber::http::HttpMethod::Get);
    EXPECT_EQ(observed.version, fiber::http::HttpVersion::HTTP_1_1);
    EXPECT_EQ(observed.path, "/interop/http1/no-body");
    EXPECT_EQ(observed.host, "localhost");
    EXPECT_TRUE(observed.body.empty());

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_server_on_loop(server, &close_promise); });
    close_future.get();
    group.stop();
    group.join();
    delete server;
}

TEST(HttpClientServerInteropTest, Http1ClientAndServerRoundTripWithBody) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    auto observed_promise = std::make_shared<std::promise<ObservedRequest>>();
    auto observed_future = observed_promise->get_future();

    fiber::async::spawn(group.at(0), [&]() {
        fiber::http::HttpHandler handler = [observed_promise](fiber::http::HttpExchange &exchange) {
            return handle_body_request(exchange, observed_promise);
        };
        return start_http_server(&group.at(0), std::move(handler), {}, nullptr, &port_promise, &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ClientRoundTripResult> client_promise;
    auto client_future = client_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_http1_client_with_body(&group.at(0), port, &client_promise); });

    ClientRoundTripResult client = client_future.get();
    ObservedRequest observed = observed_future.get();

    EXPECT_EQ(client.err, fiber::common::IoErr::None);
    EXPECT_EQ(client.status_code, 200);
    EXPECT_EQ(client.response_body, "http1-client-body");
    EXPECT_TRUE(client.body_last);

    EXPECT_EQ(observed.method, fiber::http::HttpMethod::Post);
    EXPECT_EQ(observed.version, fiber::http::HttpVersion::HTTP_1_1);
    EXPECT_EQ(observed.path, "/interop/http1/with-body");
    EXPECT_EQ(observed.host, "localhost");
    EXPECT_EQ(observed.body, "http1-client-body");

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_server_on_loop(server, &close_promise); });
    close_future.get();
    group.stop();
    group.join();
    delete server;
}

TEST(HttpClientServerInteropTest, Http2ClientAndServerRoundTripWithoutBody) {
    TempFile cert("cert", kSelfSignedCertPem);
    TempFile key("key", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key.ok);

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::http::HttpServerOptions server_options;
    server_options.tls.enabled = true;
    server_options.tls.cert_file = cert.path;
    server_options.tls.key_file = key.path;
    server_options.tls.alpn = {"h2"};

    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    auto observed_promise = std::make_shared<std::promise<ObservedRequest>>();
    auto observed_future = observed_promise->get_future();

    fiber::async::spawn(group.at(0), [&]() {
        fiber::http::HttpHandler handler = [observed_promise](fiber::http::HttpExchange &exchange) {
            return handle_no_body_request(exchange, observed_promise);
        };
        return start_http_server(&group.at(0), std::move(handler), std::move(server_options), nullptr, &port_promise,
                                 &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ClientRoundTripResult> client_promise;
    auto client_future = client_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_http2_client_no_body(&group.at(0), port, &client_promise); });

    ClientRoundTripResult client = client_future.get();
    ObservedRequest observed = observed_future.get();

    EXPECT_EQ(client.err, fiber::common::IoErr::None);
    EXPECT_EQ(client.status_code, 204);
    EXPECT_EQ(client.echoed_method, "GET");
    EXPECT_EQ(client.echoed_path, "/interop/http2/no-body");
    EXPECT_TRUE(client.response_body.empty());
    EXPECT_TRUE(client.body_last);

    EXPECT_EQ(observed.method, fiber::http::HttpMethod::Get);
    EXPECT_EQ(observed.version, fiber::http::HttpVersion::HTTP_2_0);
    EXPECT_EQ(observed.path, "/interop/http2/no-body");
    EXPECT_EQ(observed.host, "localhost");
    EXPECT_TRUE(observed.body.empty());

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_server_on_loop(server, &close_promise); });
    close_future.get();
    group.stop();
    group.join();
    delete server;
}

TEST(HttpClientServerInteropTest, Http2ClientAndServerRoundTripWithBody) {
    TempFile cert("cert", kSelfSignedCertPem);
    TempFile key("key", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key.ok);

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::http::HttpServerOptions server_options;
    server_options.tls.enabled = true;
    server_options.tls.cert_file = cert.path;
    server_options.tls.key_file = key.path;
    server_options.tls.alpn = {"h2"};

    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    auto observed_promise = std::make_shared<std::promise<ObservedRequest>>();
    auto observed_future = observed_promise->get_future();

    fiber::async::spawn(group.at(0), [&]() {
        fiber::http::HttpHandler handler = [observed_promise](fiber::http::HttpExchange &exchange) {
            return handle_body_request(exchange, observed_promise);
        };
        return start_http_server(&group.at(0), std::move(handler), std::move(server_options), nullptr, &port_promise,
                                 &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ClientRoundTripResult> client_promise;
    auto client_future = client_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_http2_client_with_body(&group.at(0), port, &client_promise); });

    ClientRoundTripResult client = client_future.get();
    ObservedRequest observed = observed_future.get();

    EXPECT_EQ(client.err, fiber::common::IoErr::None);
    EXPECT_EQ(client.status_code, 200);
    EXPECT_EQ(client.response_body, "http2-client-body");
    EXPECT_TRUE(client.body_last);

    EXPECT_EQ(observed.method, fiber::http::HttpMethod::Post);
    EXPECT_EQ(observed.version, fiber::http::HttpVersion::HTTP_2_0);
    EXPECT_EQ(observed.path, "/interop/http2/with-body");
    EXPECT_EQ(observed.host, "localhost");
    EXPECT_EQ(observed.body, "http2-client-body");

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_server_on_loop(server, &close_promise); });
    close_future.get();
    group.stop();
    group.join();
    delete server;
}

TEST(HttpClientServerInteropTest, Http2ServerEventLoopGroupDispatch) {
    TempFile cert("cert", kSelfSignedCertPem);
    TempFile key("key", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key.ok);

    fiber::event::EventLoopGroup group(2);
    group.start();

    fiber::http::HttpServerOptions server_options;
    server_options.tls.enabled = true;
    server_options.tls.cert_file = cert.path;
    server_options.tls.key_file = key.path;
    server_options.tls.alpn = {"h2"};

    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    std::atomic<bool> saw_worker_loop{false};

    fiber::async::spawn(group.at(0), [&]() {
        fiber::http::HttpHandler handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            if (&fiber::event::EventLoop::current() == &group.at(1)) {
                saw_worker_loop.store(true, std::memory_order_release);
            }
            (void)co_await send_final_header(exchange, 204, nullptr, fiber::http::HttpBodySpec::None(), true);
        };
        return start_http_server(&group.at(0), std::move(handler), std::move(server_options), &group, &port_promise,
                                 &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    for (int i = 0; i < 2; ++i) {
        std::promise<ClientRoundTripResult> client_promise;
        auto client_future = client_promise.get_future();
        fiber::async::spawn(group.at(0), [&]() {
            return run_http2_client_no_body(&group.at(0), port, &client_promise);
        });

        ClientRoundTripResult client = client_future.get();
        EXPECT_EQ(client.err, fiber::common::IoErr::None);
        EXPECT_EQ(client.status_code, 204);
        EXPECT_TRUE(client.response_body.empty());
        EXPECT_TRUE(client.body_last);
    }

    EXPECT_TRUE(saw_worker_loop.load(std::memory_order_acquire));

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_server_on_loop(server, &close_promise); });
    close_future.get();
    group.stop();
    group.join();
    delete server;
}

} // namespace
