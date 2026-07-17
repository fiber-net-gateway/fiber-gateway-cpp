#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include "async/Spawn.h"
#include "async/Task.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "http/HttpServer.h"
#include "http/HttpTransport.h"
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

std::string make_temp_path(const char *tag) {
    std::string path = "/tmp/fiber_tls_dyn_";
    path.append(tag);
    path.push_back('_');
    path.append(std::to_string(static_cast<long>(::getpid())));
    path.push_back('_');
    path.append(std::to_string(static_cast<long>(::random())));
    path.append(".pem");
    return path;
}

bool write_file(const std::string &path, std::string_view data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

struct TempFile {
    std::string path;
    bool ok = false;

    TempFile(const char *tag, std::string_view data) {
        path = make_temp_path(tag);
        ok = write_file(path, data);
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

fiber::async::Task<fiber::common::IoResult<void>> send_final_header(fiber::http::HttpExchange &exchange,
                                                                    int status_code, bool end_stream = true) {
    co_return co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = status_code,
            .end_stream = end_stream,
    });
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

struct ClientResult {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    std::string response;
    std::string negotiated_alpn;
};

DetachedTask stop_http_server(fiber::event::EventLoop *loop, fiber::http::HttpServer *server) {
    if (server) {
        server->close();
    }
    loop->stop();
    co_return;
}

DetachedTask start_http_server(fiber::event::EventLoop *loop, fiber::http::HttpHandler handler,
                               fiber::http::HttpServerOptions options, std::promise<std::uint16_t> *port_promise,
                               std::promise<fiber::http::HttpServer *> *server_promise) {
    auto *server = new fiber::http::HttpServer(*loop, std::move(handler), std::move(options));
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

DetachedTask run_tls_http1_client(fiber::event::EventLoop *loop, std::uint16_t port, std::string_view server_name,
                                  std::promise<ClientResult> *result_promise) {
    ClientResult result;

    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    auto connect_result = co_await fiber::net::TcpStream::connect(*loop, target, 5s);
    if (!connect_result) {
        result.err = connect_result.error();
        result_promise->set_value(std::move(result));
        co_return;
    }

    auto stream = std::make_unique<fiber::net::TcpStream>(std::move(*connect_result));
    fiber::net::TlsOptions tls_options{};
    tls_options.alpn = {"http/1.1"};
    tls_options.server_name.assign(server_name.data(), server_name.size());
    fiber::net::TlsContext client_ctx(std::move(tls_options), false);
    auto init_result = client_ctx.init();
    if (!init_result) {
        result.err = init_result.error();
        result_promise->set_value(std::move(result));
        co_return;
    }

    int fd = stream->release_fd();
    fiber::net::AcceptResult accept(fd, stream->remote_addr());
    auto transport_result = fiber::http::TlsTransport::create(stream->loop(), std::move(accept), client_ctx);
    if (!transport_result) {
        result.err = transport_result.error();
        result_promise->set_value(std::move(result));
        co_return;
    }
    auto transport = std::move(*transport_result);

    auto hs_result = co_await transport->handshake(5s);
    if (!hs_result) {
        result.err = hs_result.error();
        result_promise->set_value(std::move(result));
        co_return;
    }
    result.negotiated_alpn = std::string(transport->negotiated_alpn());

    const char *request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    size_t request_len = std::strlen(request);
    size_t offset = 0;
    while (offset < request_len) {
        auto write_result = co_await transport->write(request + offset, request_len - offset, 5s);
        if (!write_result || *write_result == 0) {
            result.err = write_result ? fiber::common::IoErr::BrokenPipe : write_result.error();
            result_promise->set_value(std::move(result));
            co_return;
        }
        offset += *write_result;
    }

    std::array<char, 4096> buf{};
    while (result.response.find("\r\n\r\n") == std::string::npos) {
        auto read_result = co_await transport->read(buf.data(), buf.size(), 5s);
        if (!read_result) {
            result.err = read_result.error();
            result_promise->set_value(std::move(result));
            co_return;
        }
        if (*read_result == 0) {
            break;
        }
        result.response.append(buf.data(), *read_result);
    }
    result_promise->set_value(std::move(result));
    co_return;
}

struct SelectorState {
    std::atomic<int> calls{0};
    std::atomic<bool> saw_http11{false};
    std::atomic<std::size_t> server_name_len{0};
    std::array<char, 128> server_name{};
};

fiber::net::TlsContext *select_alt_identity(void *ctx, const fiber::net::TlsClientHelloView &client_hello) noexcept {
    auto *state = static_cast<SelectorState *>(ctx);
    state->calls.fetch_add(1, std::memory_order_relaxed);
    state->saw_http11.store(client_hello.alpn.contains("http/1.1"), std::memory_order_relaxed);
    std::size_t len = std::min(client_hello.server_name.size(), state->server_name.size());
    std::memcpy(state->server_name.data(), client_hello.server_name.data(), len);
    state->server_name_len.store(len, std::memory_order_relaxed);
    if (!client_hello.server_context) {
        return nullptr;
    }
    return client_hello.server_context->find_identity_by_name("alt");
}

TEST(HttpServerTlsDynamicCertTest, SelectorCallbackCanChooseNamedIdentityUsingSniAndAlpn) {
    TempFile cert("cert", kSelfSignedCertPem);
    TempFile key("key", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key.ok);

    fiber::event::EventLoopGroup group(1);
    group.start();

    SelectorState selector_state;
    fiber::http::HttpServerOptions server_options;
    server_options.tls.enabled = true;
    server_options.tls.cert_file.clear();
    server_options.tls.key_file.clear();
    server_options.tls.identities.push_back({
            .name = "alt",
            .cert_file = cert.path,
            .key_file = key.path,
    });
    server_options.tls.identity_selector_ops.select = &select_alt_identity;
    server_options.tls.identity_selector_ops.ctx = &selector_state;

    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            (void) co_await send_final_header(exchange, 204, true);
            co_return;
        };
        return start_http_server(&group.at(0), handler, server_options, &port_promise, &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ClientResult> client_promise;
    auto client_future = client_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_tls_http1_client(&group.at(0), port, "foo.a.com", &client_promise); });

    ClientResult client = client_future.get();
    EXPECT_EQ(client.err, fiber::common::IoErr::None);
    EXPECT_EQ(client.negotiated_alpn, "http/1.1");
    EXPECT_NE(client.response.find("204"), std::string::npos);
    EXPECT_EQ(selector_state.calls.load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(selector_state.saw_http11.load(std::memory_order_relaxed));
    const std::size_t name_len = selector_state.server_name_len.load(std::memory_order_relaxed);
    EXPECT_EQ(std::string_view(selector_state.server_name.data(), name_len), "foo.a.com");

    fiber::async::spawn(group.at(0), [&]() { return stop_http_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(HttpServerTlsDynamicCertTest, DefaultIdentityCanServeWithoutCustomSelector) {
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

    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            (void) co_await send_final_header(exchange, 204, true);
            co_return;
        };
        return start_http_server(&group.at(0), handler, server_options, &port_promise, &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ClientResult> client_promise;
    auto client_future = client_promise.get_future();
    fiber::async::spawn(group.at(0),
                        [&]() { return run_tls_http1_client(&group.at(0), port, "bar.b.com", &client_promise); });

    ClientResult client = client_future.get();
    EXPECT_EQ(client.err, fiber::common::IoErr::None);
    EXPECT_EQ(client.negotiated_alpn, "http/1.1");
    EXPECT_NE(client.response.find("204"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() { return stop_http_server(&group.at(0), server); });
    group.join();
    delete server;
}

// TLS HTTP/1 client that POSTs a request body and reads the entire response
// (headers + body) until EOF. Used to verify chunked response framing over TLS.
DetachedTask run_tls_http1_client_post_and_read_all(fiber::event::EventLoop *loop, std::uint16_t port,
                                                    std::string_view server_name, std::string_view request_body,
                                                    std::promise<ClientResult> *result_promise) {
    ClientResult result;

    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    auto connect_result = co_await fiber::net::TcpStream::connect(*loop, target, 5s);
    if (!connect_result) {
        result.err = connect_result.error();
        result_promise->set_value(std::move(result));
        co_return;
    }

    auto stream = std::make_unique<fiber::net::TcpStream>(std::move(*connect_result));
    fiber::net::TlsOptions tls_options{};
    tls_options.alpn = {"http/1.1"};
    tls_options.server_name.assign(server_name.data(), server_name.size());
    fiber::net::TlsContext client_ctx(std::move(tls_options), false);
    auto init_result = client_ctx.init();
    if (!init_result) {
        result.err = init_result.error();
        result_promise->set_value(std::move(result));
        co_return;
    }

    int fd = stream->release_fd();
    fiber::net::AcceptResult accept(fd, stream->remote_addr());
    auto transport_result = fiber::http::TlsTransport::create(stream->loop(), std::move(accept), client_ctx);
    if (!transport_result) {
        result.err = transport_result.error();
        result_promise->set_value(std::move(result));
        co_return;
    }
    auto transport = std::move(*transport_result);

    auto hs_result = co_await transport->handshake(5s);
    if (!hs_result) {
        result.err = hs_result.error();
        result_promise->set_value(std::move(result));
        co_return;
    }
    result.negotiated_alpn = std::string(transport->negotiated_alpn());

    std::string request =
            "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(request_body.size()) +
            "\r\nConnection: close\r\n\r\n" + std::string(request_body);
    std::size_t offset = 0;
    while (offset < request.size()) {
        auto write_result = co_await transport->write(request.data() + offset, request.size() - offset, 5s);
        if (!write_result || *write_result == 0) {
            result.err = write_result ? fiber::common::IoErr::BrokenPipe : write_result.error();
            result_promise->set_value(std::move(result));
            co_return;
        }
        offset += *write_result;
    }

    std::array<char, 4096> buf{};
    for (;;) {
        auto read_result = co_await transport->read(buf.data(), buf.size(), 5s);
        if (!read_result) {
            result.err = read_result.error();
            result_promise->set_value(std::move(result));
            co_return;
        }
        if (*read_result == 0) {
            break;
        }
        result.response.append(buf.data(), *read_result);
    }
    result_promise->set_value(std::move(result));
    co_return;
}

// End-to-end over TLS: server sends a chunked response via write_body(IoBufChain),
// which builds [prefix][body][suffix] and feeds TlsTransport::writev (the coalesce
// path). The client verifies the exact chunked wire framing survives, proving the
// two parts compose correctly on the real HTTPS path.
TEST(HttpServerTlsDynamicCertTest, ChunkedResponseEchoedOverTls) {
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

    const std::string body = "Hello, TLS chunked world!"; // 25 bytes = 0x19
    auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        auto read_result = co_await exchange.read_body(8192);
        if (!read_result) {
            co_return;
        }
        auto header_result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 200,
                .body = fiber::http::ResponseBodySpec::Chunked(),
                .end_stream = false,
        });
        if (!header_result) {
            co_return;
        }
        co_await exchange.write_body(std::move(*read_result));
        co_return;
    };

    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        return start_http_server(&group.at(0), handler, server_options, &port_promise, &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<ClientResult> client_promise;
    auto client_future = client_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_tls_http1_client_post_and_read_all(&group.at(0), port, "bar.b.com", body, &client_promise);
    });

    ClientResult client = client_future.get();
    EXPECT_EQ(client.err, fiber::common::IoErr::None) << "TLS client error";
    // One chunk (hex size + CRLF + body) + folded final terminator, unmodified by
    // coalescing:  "19\r\n" + body + "\r\n0\r\n\r\n".
    std::string expected_body_section = "19\r\n" + body + "\r\n0\r\n\r\n";
    EXPECT_NE(client.response.find("200"), std::string::npos) << client.response;
    EXPECT_NE(client.response.find("Transfer-Encoding: chunked"), std::string::npos) << client.response;
    EXPECT_NE(client.response.find(expected_body_section), std::string::npos) << client.response;

    fiber::async::spawn(group.at(0), [&]() { return stop_http_server(&group.at(0), server); });
    group.join();
    delete server;
}

} // namespace
