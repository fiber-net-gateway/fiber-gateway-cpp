#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/ClientHttp1Exchange.h>
#include <fiber/http/ClientHttp2Exchange.h>
#include <fiber/http/ClientHttpConnector.h>
#include <fiber/http/ClientHttpExchange.h>
#include <fiber/http/Http1ClientConnection.h>
#include <fiber/http/Http2ClientConnection.h>
#include <fiber/http/HttpServer.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/TcpStream.h>
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

struct TempFile {
    std::string path;
    bool ok = false;

    TempFile(const char *tag, std::string_view data) {
        static std::atomic<std::uint32_t> next_id{1};
        path = "/tmp/fiber_client_exchange_";
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

// ---------------------------------------------------------------------------
// Server side: one echo handler shared by every protocol under test.
// ---------------------------------------------------------------------------

struct ObservedRequest {
    std::string host;
    std::string method;
    std::string path;
    std::string body;
};

fiber::async::Task<void> handle_echo(fiber::http::HttpExchange &exchange,
                                     std::shared_ptr<std::promise<ObservedRequest>> observed) {
    ObservedRequest seen;
    seen.method.assign(exchange.method_view());
    seen.path.assign(exchange.uri().path.data(), exchange.uri().path.size());
    const std::string_view host = exchange.request_headers().get("host");
    seen.host.assign(host.data(), host.size());

    for (;;) {
        auto chunk = co_await exchange.read_body(4096);
        if (!chunk) {
            co_return;
        }
        const bool last = chunk->complete();
        seen.body.append(chain_to_string(std::move(*chunk)));
        if (last) {
            break;
        }
    }
    if (observed) {
        observed->set_value(seen);
    }

    const std::string payload = "echo:" + seen.path;
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("content-type", "text/plain");
    headers.set("x-echo-method", seen.method);
    auto header_result = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::ContentLength(payload.size()),
            .end_stream = false,
    });
    if (!header_result) {
        co_return;
    }
    (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size(), true);
}

DetachedTask start_http_server(fiber::event::EventLoop *loop, fiber::http::HttpHandler handler,
                               fiber::http::HttpServerOptions options, std::promise<std::uint16_t> *port_promise,
                               std::promise<fiber::http::HttpServer *> *server_promise) {
    auto *server = new fiber::http::HttpServer(*loop, std::move(handler), std::move(options), nullptr);
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

DetachedTask close_server_on_loop(fiber::http::HttpServer *server, std::promise<void> *done) {
    if (server) {
        co_await server->shutdown_and_wait();
    }
    done->set_value();
    co_return;
}

// ---------------------------------------------------------------------------
// The point of the whole exercise: one request routine, any protocol.
// ---------------------------------------------------------------------------

struct RoundTrip {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    fiber::http::HttpVersion version = fiber::http::HttpVersion::HTTP_1_1;
    int status_code = 0;
    std::string echoed_method;
    std::string body;
    bool saw_trailer = false;
    std::string trailer_value;
    // read_header after the last block must report the end with a null head, on every protocol.
    bool ends_with_null_head = false;
};

fiber::async::Task<RoundTrip> run_request(fiber::http::ClientHttpExchange exchange,
                                          const fiber::http::ClientRequestHead &head, std::string_view request_body) {
    RoundTrip out;
    out.version = exchange.version();

    const bool end_stream = request_body.empty();
    auto sent = co_await exchange.send_header(head, end_stream, 5s);
    if (!sent) {
        out.err = sent.error();
        co_return out;
    }
    if (!end_stream) {
        auto written = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(request_body.data()),
                                                   request_body.size(), true, 5s);
        if (!written) {
            out.err = written.error();
            co_return out;
        }
    }

    const fiber::http::ClientResponseHead *final_head = nullptr;
    for (;;) {
        auto header = co_await exchange.read_header(5s);
        if (!header) {
            out.err = header.error();
            co_return out;
        }
        if (*header == nullptr) {
            out.err = fiber::common::IoErr::ConnReset;
            co_return out;
        }
        if (!(*header)->is_informational()) {
            final_head = *header;
            break;
        }
    }
    out.status_code = final_head->status_code;
    out.echoed_method = std::string(final_head->headers.get("x-echo-method"));

    for (;;) {
        auto chunk = co_await exchange.read_body(64, 5s);
        if (!chunk) {
            out.err = chunk.error();
            co_return out;
        }
        const bool last = chunk->complete();
        out.body.append(chain_to_string(std::move(*chunk)));
        if (last) {
            break;
        }
    }

    // Trailers, then end-of-headers. Identical shape on HTTP/1, HTTP/2 and HTTP/3.
    auto trailing = co_await exchange.read_header(5s);
    if (!trailing) {
        out.err = trailing.error();
        co_return out;
    }
    if (*trailing != nullptr) {
        out.saw_trailer = (*trailing)->kind == fiber::http::OutgoingHeaderKind::Trailer;
        out.trailer_value = std::string((*trailing)->headers.get("x-checksum"));
        auto after = co_await exchange.read_header(5s);
        out.ends_with_null_head = after.has_value() && *after == nullptr;
    } else {
        out.ends_with_null_head = true;
    }
    co_return out;
}

DetachedTask run_over_http1(fiber::event::EventLoop *loop, std::uint16_t port, std::promise<RoundTrip> *promise) {
    fiber::http::Http1ClientConnection connection(*loop);
    auto connected =
            co_await connection.connect(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port), 5s);
    if (!connected) {
        RoundTrip failed;
        failed.err = connected.error();
        promise->set_value(std::move(failed));
        co_return;
    }

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    fiber::http::ClientHttp1Exchange http1(connection, pool);

    fiber::http::ClientRequestHead head;
    head.method = fiber::http::HttpMethod::Post;
    head.scheme = "http";
    head.authority = "unified.example";
    head.path = "/unified";
    head.headers = &headers;
    head.body = fiber::http::HttpBodySpec::ContentLength(5);

    auto result = co_await run_request(fiber::http::ClientHttpExchange(http1), head, "hello");
    promise->set_value(std::move(result));
    // The exchange is still active here; it must release the connection before close(), which is
    // exactly what the reverse-order destruction of these locals does.
    co_return;
}

struct Http2RunState {
    std::atomic_bool done{false};
};

DetachedTask wait_http2_closed(fiber::http::Http2ClientConnection *connection, Http2RunState *state) {
    (void) co_await connection->wait_closed();
    state->done.store(true, std::memory_order_release);
    co_return;
}

DetachedTask run_over_http2(fiber::event::EventLoop *loop, std::uint16_t port, std::promise<RoundTrip> *promise) {
    fiber::http::HttpClientTlsOptions tls;
    tls.server_name = "localhost";

    fiber::http::Http2ClientConnection connection(*loop);
    auto connected =
            co_await connection.connect(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port), 5s, tls);
    if (!connected) {
        RoundTrip failed;
        failed.err = connected.error();
        promise->set_value(std::move(failed));
        co_return;
    }

    auto state = std::make_shared<Http2RunState>();
    fiber::async::spawn(*loop, [conn = &connection, raw = state.get()]() { return wait_http2_closed(conn, raw); });

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    fiber::http::ClientHttp2Exchange http2(connection, pool);

    fiber::http::ClientRequestHead head;
    head.method = fiber::http::HttpMethod::Post;
    head.scheme = "https";
    head.authority = "unified.example";
    head.path = "/unified";
    head.headers = &headers;
    head.body = fiber::http::HttpBodySpec::ContentLength(5);

    auto result = co_await run_request(fiber::http::ClientHttpExchange(http2), head, "hello");
    promise->set_value(std::move(result));

    connection.shutdown();
    for (int i = 0; i < 200 && !state->done.load(std::memory_order_acquire); ++i) {
        co_await fiber::async::sleep(1ms);
    }
    co_return;
}

// ---------------------------------------------------------------------------
// A raw HTTP/1 server, for cases that need exact control of the response bytes.
// ---------------------------------------------------------------------------

struct RawServerState {
    std::string response;
    std::atomic_bool ready{false};
    std::string request;
    std::promise<std::string> request_promise;
};

DetachedTask run_raw_http1_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                                  RawServerState *state) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions listen_options{};
    auto bound = listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), listen_options);
    if (!bound) {
        port_promise->set_value(0);
        state->request_promise.set_value({});
        co_return;
    }
    auto port = resolve_port(listener.fd());
    port_promise->set_value(port ? *port : 0);

    auto accepted = co_await listener.accept();
    if (!accepted) {
        state->request_promise.set_value({});
        co_return;
    }
    fiber::net::TcpStream stream(*loop, accepted->release_fd(), accepted->take_peer());

    std::string request;
    std::array<std::uint8_t, 4096> buffer{};
    while (request.find("\r\n\r\n") == std::string::npos) {
        auto read = co_await stream.read(buffer.data(), buffer.size(), 5s);
        if (!read || *read == 0) {
            break;
        }
        request.append(reinterpret_cast<const char *>(buffer.data()), *read);
    }
    state->request_promise.set_value(request);

    std::size_t sent = 0;
    while (sent < state->response.size()) {
        auto written = co_await stream.write(state->response.data() + sent, state->response.size() - sent, 5s);
        if (!written || *written == 0) {
            break;
        }
        sent += *written;
    }
    // Hold the connection open long enough for the client to finish reading.
    co_await fiber::async::sleep(50ms);
    stream.close();
    listener.close();
    co_return;
}

struct Http1RawOutcome {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    int status_code = 0;
    std::string body;
    bool saw_trailer = false;
    std::string trailer_value;
    std::string trailer_from_accessor;
    bool ends_with_null_head = false;
};

DetachedTask run_http1_against_raw(fiber::event::EventLoop *loop, std::uint16_t port,
                                   std::promise<Http1RawOutcome> *promise) {
    Http1RawOutcome out;
    fiber::http::Http1ClientConnection connection(*loop);
    auto connected =
            co_await connection.connect(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port), 5s);
    if (!connected) {
        out.err = connected.error();
        promise->set_value(std::move(out));
        co_return;
    }

    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    // Deliberately wrong: ClientRequestHead::authority must win over this.
    headers.set("host", "ignored.example");

    fiber::http::ClientHttp1Exchange exchange(connection, pool);
    fiber::http::ClientRequestHead head;
    head.method = fiber::http::HttpMethod::Get;
    head.path = "/raw";
    head.authority = "authority.example";
    head.headers = &headers;

    auto sent = co_await exchange.send_header(head, true, 5s);
    if (!sent) {
        out.err = sent.error();
        promise->set_value(std::move(out));
        co_return;
    }

    auto header = co_await exchange.read_header(5s);
    if (!header || *header == nullptr) {
        out.err = header ? fiber::common::IoErr::ConnReset : header.error();
        promise->set_value(std::move(out));
        co_return;
    }
    out.status_code = (*header)->status_code;

    for (;;) {
        auto chunk = co_await exchange.read_body(64, 5s);
        if (!chunk) {
            out.err = chunk.error();
            promise->set_value(std::move(out));
            co_return;
        }
        const bool last = chunk->complete();
        out.body.append(chain_to_string(std::move(*chunk)));
        if (last) {
            break;
        }
    }

    auto trailing = co_await exchange.read_header(5s);
    if (!trailing) {
        out.err = trailing.error();
        promise->set_value(std::move(out));
        co_return;
    }
    if (*trailing != nullptr) {
        out.saw_trailer = (*trailing)->kind == fiber::http::OutgoingHeaderKind::Trailer;
        out.trailer_value = std::string((*trailing)->headers.get("x-checksum"));
    }
    out.trailer_from_accessor = std::string(exchange.response_trailers().get("x-checksum"));

    auto after = co_await exchange.read_header(5s);
    out.ends_with_null_head = after.has_value() && *after == nullptr;

    promise->set_value(std::move(out));
    co_return;
}

// ---------------------------------------------------------------------------
// ClientHttpConnector
// ---------------------------------------------------------------------------

struct ConnectorOutcome {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    fiber::http::HttpProtocol first = fiber::http::HttpProtocol::Http1;
    fiber::http::HttpProtocol second = fiber::http::HttpProtocol::Http1;
    bool second_reused = false;
    int first_status = 0;
    int second_status = 0;
    bool hint_recorded = false;
};

fiber::async::Task<fiber::common::IoResult<int>> drive_pooled_request(fiber::http::PooledClientHttpExchange &pooled,
                                                                      std::string_view path) {
    fiber::http::ClientHttpExchange exchange = pooled.exchange();
    fiber::mem::BufPool header_pool;
    fiber::http::HttpHeaders headers(header_pool);

    fiber::http::ClientRequestHead head;
    head.method = fiber::http::HttpMethod::Get;
    head.scheme = "https";
    head.authority = "localhost";
    head.path = path;
    head.headers = &headers;

    auto sent = co_await exchange.send_header(head, true, 5s);
    if (!sent) {
        co_return std::unexpected(sent.error());
    }
    const fiber::http::ClientResponseHead *final_head = nullptr;
    for (;;) {
        auto header = co_await exchange.read_header(5s);
        if (!header) {
            co_return std::unexpected(header.error());
        }
        if (*header == nullptr) {
            co_return std::unexpected(fiber::common::IoErr::ConnReset);
        }
        if (!(*header)->is_informational()) {
            final_head = *header;
            break;
        }
    }
    const int status = final_head->status_code;
    for (;;) {
        auto chunk = co_await exchange.read_body(256, 5s);
        if (!chunk) {
            co_return std::unexpected(chunk.error());
        }
        if (chunk->complete()) {
            break;
        }
    }
    co_return status;
}

DetachedTask run_connector(fiber::http::StealableHttp1ConnectionPoolSet *http1_pool,
                           fiber::http::LocalHttp2ConnectionPoolSet *http2_pool, std::uint16_t port,
                           fiber::http::HttpProtocolPreference preference, std::promise<ConnectorOutcome> *promise) {
    ConnectorOutcome out;
    fiber::http::ClientHttpConnector connector(*http1_pool, *http2_pool);

    const auto key = fiber::http::HttpConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port,
                                                                  fiber::http::HttpConnectionGroupKey::Scheme::Https);
    fiber::http::HttpClientTlsOptions tls;
    tls.server_name = "localhost";

    fiber::http::HttpClientAcquireOptions options;
    options.preference = preference;
    options.tls = &tls;
    options.happy.total_timeout = 5s;
    options.pool_timeout = 5s;

    fiber::mem::BufPool pool;
    {
        fiber::http::PooledClientHttpExchange pooled;
        auto acquired = co_await connector.acquire(key, pool, options, pooled);
        if (!acquired) {
            out.err = acquired.error();
            promise->set_value(std::move(out));
            co_return;
        }
        out.first = pooled.protocol();
        auto status = co_await drive_pooled_request(pooled, "/connector/first");
        if (!status) {
            out.err = status.error();
            promise->set_value(std::move(out));
            co_return;
        }
        out.first_status = *status;
    }

    out.hint_recorded = connector.hints().lookup(key, fiber::event::EventLoop::current().now()).has_value();

    {
        fiber::http::PooledClientHttpExchange pooled;
        auto acquired = co_await connector.acquire(key, pool, options, pooled);
        if (!acquired) {
            out.err = acquired.error();
            promise->set_value(std::move(out));
            co_return;
        }
        out.second = pooled.protocol();
        out.second_reused = pooled.reused();
        auto status = co_await drive_pooled_request(pooled, "/connector/second");
        if (!status) {
            out.err = status.error();
            promise->set_value(std::move(out));
            co_return;
        }
        out.second_status = *status;
    }

    promise->set_value(std::move(out));
    co_return;
}

} // namespace

TEST(ClientHttpExchangeTest, SameRequestCodeRunsOverHttp1AndHttp2) {
    TempFile cert("cert", kSelfSignedCertPem);
    TempFile key("key", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key.ok);

    fiber::event::EventLoopGroup group(1);
    group.start();

    // Plaintext server for HTTP/1.
    std::promise<std::uint16_t> h1_port_promise;
    std::promise<fiber::http::HttpServer *> h1_server_promise;
    auto h1_port_future = h1_port_promise.get_future();
    auto h1_server_future = h1_server_promise.get_future();
    auto h1_observed = std::make_shared<std::promise<ObservedRequest>>();
    auto h1_observed_future = h1_observed->get_future();
    fiber::async::spawn(group.at(0), [&]() {
        fiber::http::HttpHandler handler = [h1_observed](fiber::http::HttpExchange &exchange) {
            return handle_echo(exchange, h1_observed);
        };
        return start_http_server(&group.at(0), std::move(handler), fiber::http::HttpServerOptions{}, &h1_port_promise,
                                 &h1_server_promise);
    });
    auto *h1_server = h1_server_future.get();
    ASSERT_NE(h1_server, nullptr);
    const std::uint16_t h1_port = h1_port_future.get();
    ASSERT_NE(h1_port, 0);

    // TLS server for HTTP/2; HttpServer always offers {h2, http/1.1}.
    fiber::net::TlsCredentialOptions credential_options{};
    credential_options.certificate_chain = fiber::net::TlsPemSource::from_file(cert.path);
    credential_options.private_key = fiber::net::TlsPemSource::from_file(key.path);
    auto credential = fiber::net::TlsCredential::create(credential_options);
    ASSERT_TRUE(credential);
    fiber::http::HttpServerOptions tls_options;
    tls_options.tls.configure_callback = &fiber::net::configure_tls_with_credential;
    tls_options.tls.configure_ctx = credential->get();

    std::promise<std::uint16_t> h2_port_promise;
    std::promise<fiber::http::HttpServer *> h2_server_promise;
    auto h2_port_future = h2_port_promise.get_future();
    auto h2_server_future = h2_server_promise.get_future();
    auto h2_observed = std::make_shared<std::promise<ObservedRequest>>();
    auto h2_observed_future = h2_observed->get_future();
    fiber::async::spawn(group.at(0), [&]() {
        fiber::http::HttpHandler handler = [h2_observed](fiber::http::HttpExchange &exchange) {
            return handle_echo(exchange, h2_observed);
        };
        return start_http_server(&group.at(0), std::move(handler), std::move(tls_options), &h2_port_promise,
                                 &h2_server_promise);
    });
    auto *h2_server = h2_server_future.get();
    ASSERT_NE(h2_server, nullptr);
    const std::uint16_t h2_port = h2_port_future.get();
    ASSERT_NE(h2_port, 0);

    std::promise<RoundTrip> h1_promise;
    auto h1_future = h1_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_over_http1(&group.at(0), h1_port, &h1_promise); });
    RoundTrip over_http1 = h1_future.get();

    std::promise<RoundTrip> h2_promise;
    auto h2_future = h2_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_over_http2(&group.at(0), h2_port, &h2_promise); });
    RoundTrip over_http2 = h2_future.get();

    EXPECT_EQ(over_http1.err, fiber::common::IoErr::None);
    EXPECT_EQ(over_http2.err, fiber::common::IoErr::None);

    // The whole point: identical observable results from one request routine.
    EXPECT_EQ(over_http1.status_code, 200);
    EXPECT_EQ(over_http2.status_code, 200);
    EXPECT_EQ(over_http1.body, "echo:/unified");
    EXPECT_EQ(over_http2.body, "echo:/unified");
    EXPECT_EQ(over_http1.echoed_method, "POST");
    EXPECT_EQ(over_http2.echoed_method, "POST");
    EXPECT_TRUE(over_http1.ends_with_null_head);
    EXPECT_TRUE(over_http2.ends_with_null_head);

    // ...and the version each ran on is still reported truthfully.
    EXPECT_EQ(over_http1.version, fiber::http::HttpVersion::HTTP_1_1);
    EXPECT_EQ(over_http2.version, fiber::http::HttpVersion::HTTP_2_0);

    // ClientRequestHead::authority reaches the server as Host / :authority on both.
    const ObservedRequest h1_seen = h1_observed_future.get();
    const ObservedRequest h2_seen = h2_observed_future.get();
    EXPECT_EQ(h1_seen.host, "unified.example");
    EXPECT_EQ(h2_seen.host, "unified.example");
    EXPECT_EQ(h1_seen.body, "hello");
    EXPECT_EQ(h2_seen.body, "hello");

    std::promise<void> h1_closed;
    auto h1_closed_future = h1_closed.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_server_on_loop(h1_server, &h1_closed); });
    h1_closed_future.get();
    std::promise<void> h2_closed;
    auto h2_closed_future = h2_closed.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_server_on_loop(h2_server, &h2_closed); });
    h2_closed_future.get();
    delete h1_server;
    delete h2_server;

    group.stop();
    group.join();
}

TEST(ClientHttpExchangeTest, Http1AuthorityOverridesHostHeader) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    RawServerState state;
    state.response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
    auto request_future = state.request_promise.get_future();
    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_raw_http1_server(&group.at(0), &port_promise, &state); });
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<Http1RawOutcome> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_http1_against_raw(&group.at(0), port, &promise); });

    const Http1RawOutcome outcome = future.get();
    const std::string request = request_future.get();

    EXPECT_EQ(outcome.err, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.status_code, 200);
    EXPECT_EQ(outcome.body, "hi");
    // Exactly one Host line, carrying the authority rather than the header the caller left behind.
    EXPECT_NE(request.find("Host: authority.example\r\n"), std::string::npos);
    EXPECT_EQ(request.find("ignored.example"), std::string::npos);
    EXPECT_EQ(request.find("Host: "), request.rfind("Host: "));
    // No trailers here, so the block after the body is the end-of-headers marker.
    EXPECT_FALSE(outcome.saw_trailer);
    EXPECT_TRUE(outcome.ends_with_null_head);

    group.stop();
    group.join();
}

TEST(ClientHttpExchangeTest, Http1TrailersArriveThroughReadHeader) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    RawServerState state;
    state.response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "5\r\nhello\r\n"
                     "0\r\nx-checksum: abc123\r\n\r\n";
    auto request_future = state.request_promise.get_future();
    std::promise<std::uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_raw_http1_server(&group.at(0), &port_promise, &state); });
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    std::promise<Http1RawOutcome> promise;
    auto future = promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_http1_against_raw(&group.at(0), port, &promise); });

    const Http1RawOutcome outcome = future.get();
    (void) request_future.get();

    EXPECT_EQ(outcome.err, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.body, "hello");
    // The trailer block comes back from read_header with kind Trailer, exactly as HTTP/2 does.
    EXPECT_TRUE(outcome.saw_trailer);
    EXPECT_EQ(outcome.trailer_value, "abc123");
    EXPECT_EQ(outcome.trailer_from_accessor, "abc123");
    // And the block after it is the end-of-headers marker, not the trailer again.
    EXPECT_TRUE(outcome.ends_with_null_head);

    group.stop();
    group.join();
}

TEST(ClientHttpConnectorTest, AutoNegotiatesHttp2AndReusesItThroughTheHint) {
    TempFile cert("cert", kSelfSignedCertPem);
    TempFile key("key", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key.ok);

    fiber::event::EventLoopGroup server_group(1);
    server_group.start();

    fiber::net::TlsCredentialOptions credential_options{};
    credential_options.certificate_chain = fiber::net::TlsPemSource::from_file(cert.path);
    credential_options.private_key = fiber::net::TlsPemSource::from_file(key.path);
    auto credential = fiber::net::TlsCredential::create(credential_options);
    ASSERT_TRUE(credential);
    fiber::http::HttpServerOptions server_options;
    server_options.tls.configure_callback = &fiber::net::configure_tls_with_credential;
    server_options.tls.configure_ctx = credential->get();

    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(server_group.at(0), [&]() {
        fiber::http::HttpHandler handler = [](fiber::http::HttpExchange &exchange) {
            return handle_echo(exchange, nullptr);
        };
        return start_http_server(&server_group.at(0), std::move(handler), std::move(server_options), &port_promise,
                                 &server_promise);
    });
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    fiber::event::EventLoopGroup group(1);
    fiber::http::StealableHttp1ConnectionPoolSet http1_pool(group);
    fiber::http::LocalHttp2ConnectionPoolSet http2_pool(group);
    ASSERT_TRUE(http1_pool.init());
    ASSERT_TRUE(http2_pool.init());
    group.start();

    std::promise<ConnectorOutcome> auto_promise;
    auto auto_future = auto_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_connector(&http1_pool, &http2_pool, port, fiber::http::HttpProtocolPreference::Auto, &auto_promise);
    });
    const ConnectorOutcome negotiated = auto_future.get();

    EXPECT_EQ(negotiated.err, fiber::common::IoErr::None);
    // The server offers h2 first, so Auto lands on HTTP/2 without the caller saying so.
    EXPECT_EQ(negotiated.first, fiber::http::HttpProtocol::Http2);
    EXPECT_EQ(negotiated.first_status, 200);
    EXPECT_TRUE(negotiated.hint_recorded);
    // The second request follows the hint into the HTTP/2 pool and reuses the connection, so it
    // never negotiates again.
    EXPECT_EQ(negotiated.second, fiber::http::HttpProtocol::Http2);
    EXPECT_TRUE(negotiated.second_reused);
    EXPECT_EQ(negotiated.second_status, 200);

    std::promise<ConnectorOutcome> h1_promise;
    auto h1_future = h1_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_connector(&http1_pool, &http2_pool, port, fiber::http::HttpProtocolPreference::Http1Only,
                             &h1_promise);
    });
    const ConnectorOutcome forced = h1_future.get();

    EXPECT_EQ(forced.err, fiber::common::IoErr::None);
    // Same server, same key: offering only http/1.1 pins the same origin to HTTP/1.
    EXPECT_EQ(forced.first, fiber::http::HttpProtocol::Http1);
    EXPECT_EQ(forced.first_status, 200);
    EXPECT_EQ(forced.second, fiber::http::HttpProtocol::Http1);
    EXPECT_TRUE(forced.second_reused);
    EXPECT_EQ(forced.second_status, 200);

    std::promise<void> shutdown_done;
    auto shutdown_future = shutdown_done.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await http2_pool.shutdown_async();
        co_await http1_pool.shutdown_async();
        shutdown_done.set_value();
    });
    shutdown_future.get();

    group.stop();
    group.join();

    std::promise<void> closed;
    auto closed_future = closed.get_future();
    fiber::async::spawn(server_group.at(0), [&]() { return close_server_on_loop(server, &closed); });
    closed_future.get();
    delete server;
    server_group.stop();
    server_group.join();
}
