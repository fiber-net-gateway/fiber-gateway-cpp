#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/ClientHttp2Exchange.h>
#include <fiber/http/Http2ClientConnection.h>
#include <fiber/http/HttpClientTlsOptions.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http/Server.h>
#include <fiber/http/endpoint/Http2Endpoint.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsServerHandshakeConfig.h>

namespace {

using fiber::async::DetachedTask;
using fiber::http::Http2Endpoint;
using fiber::http::Server;
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

fiber::async::Task<void> write_text(fiber::http::HttpExchange &exchange, int status, std::string_view body) {
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", "text/plain");
    auto sent = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = status,
            .headers = &headers,
            .body = fiber::http::ResponseBodySpec::ContentLength(body.size()),
            .end_stream = body.empty(),
    });
    if (!sent || body.empty()) {
        co_return;
    }
    (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
    co_return;
}

struct RunningServer {
    std::unique_ptr<Server> server{};
    Http2Endpoint *endpoint = nullptr;
    std::uint16_t port = 0;
    std::future<void> serve_done{};

    RunningServer() = default;
    RunningServer(RunningServer &&) = default;
    RunningServer &operator=(RunningServer &&) = default;

    void stop_and_join() {
        server->stop();
        serve_done.get();
    }

    ~RunningServer() {
        if (server && server->state() != Server::State::Stopped && server->state() != Server::State::Created) {
            stop_and_join();
        }
    }
};

RunningServer start_server(fiber::event::EventLoopGroup &group, Http2Endpoint::Options options,
                           fiber::event::EventLoopGroup *workers = nullptr) {
    RunningServer running;
    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{}, workers);
    options.address = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0);
    running.endpoint = running.server->add_endpoint<Http2Endpoint>(std::move(options));
    if (running.endpoint == nullptr || !running.server->start().has_value()) {
        running.server.reset();
        return running;
    }
    running.port = running.endpoint->local_addr().port();

    auto done = std::make_shared<std::promise<void>>();
    running.serve_done = done->get_future();
    Server *server = running.server.get();
    fiber::async::spawn(group.at(0), [server, done]() -> DetachedTask {
        co_await server->serve();
        done->set_value();
        co_return;
    });
    return running;
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

// ---- HTTP/2 client plumbing ----

struct Http2RunState {
    std::atomic<bool> done{false};
    std::atomic<fiber::common::IoErr> err{fiber::common::IoErr::None};
};

DetachedTask run_connection(fiber::http::Http2ClientConnection *connection, Http2RunState *state) {
    auto result = co_await connection->wait_closed();
    state->err.store(result ? fiber::common::IoErr::None : result.error(), std::memory_order_release);
    state->done.store(true, std::memory_order_release);
    co_return;
}

struct ClientResult {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    int status_code = 0;
    std::string alpn{};
    std::string body{};
};

// Opens one h2 session, runs `requests` GETs on it, and reports the last
// response. `hold` keeps the session open until the caller releases it, which
// is how the drain tests observe a GOAWAY mid-session.
DetachedTask run_http2_client(fiber::event::EventLoop *loop, std::uint16_t port, std::promise<ClientResult> *promise,
                              std::shared_ptr<std::promise<void>> opened = {}, std::shared_future<void> hold = {}) {
    ClientResult result;
    fiber::http::HttpClientTlsOptions tls;
    tls.server_name = "localhost";

    fiber::http::Http2ClientConnection connection(*loop);
    auto connected =
            co_await connection.connect(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port), 5s, tls);
    if (!connected) {
        result.err = connected.error();
        promise->set_value(std::move(result));
        co_return;
    }

    auto run_state = std::make_shared<Http2RunState>();
    fiber::async::spawn(*loop, [conn = &connection, state = run_state.get()]() { return run_connection(conn, state); });

    fiber::mem::BufPool pool;
    {
        fiber::http::ClientHttp2Exchange exchange(connection, pool);
        auto sent = co_await exchange.send_header(
                {
                        .method = fiber::http::HttpMethod::Get,
                        .path = "/first",
                        .scheme = "https",
                        .authority = "localhost",
                },
                true);
        if (!sent) {
            result.err = sent.error();
        } else {
            auto header = co_await exchange.read_header();
            if (!header) {
                result.err = header.error();
            } else {
                result.status_code = (*header)->status_code;
                auto body = co_await exchange.read_body(64);
                if (body) {
                    result.body = chain_to_string(std::move(*body));
                }
            }
        }
    }

    if (opened) {
        opened->set_value();
    }
    if (hold.valid()) {
        // Park until the test says so, so the session is still open when the
        // server starts draining.
        while (hold.wait_for(0ms) != std::future_status::ready) {
            co_await fiber::async::sleep(2ms);
        }
    }

    connection.shutdown();
    for (int i = 0; i < 2500 && !run_state->done.load(std::memory_order_acquire); ++i) {
        co_await fiber::async::sleep(2ms);
    }
    promise->set_value(std::move(result));
    co_return;
}

ClientResult http2_round_trip(fiber::event::EventLoopGroup &client_group, std::uint16_t port) {
    std::promise<ClientResult> promise;
    auto future = promise.get_future();
    fiber::async::spawn(client_group.at(0), [&client_group, port, &promise]() {
        return run_http2_client(&client_group.at(0), port, &promise);
    });
    return future.get();
}

// ---- plain HTTP/1 client, for the negotiated-down path ----

int connect_to(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    sockaddr_storage storage{};
    socklen_t len = 0;
    if (!target.to_sockaddr(storage, len) || ::connect(fd, reinterpret_cast<sockaddr *>(&storage), len) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string recv_all(int fd) {
    std::string out;
    char buffer[4096];
    for (;;) {
        ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        out.append(buffer, static_cast<std::size_t>(received));
    }
    return out;
}

std::unique_ptr<fiber::net::TlsCredential> make_credential() {
    fiber::net::TlsCredentialOptions options{};
    options.certificate_chain = fiber::net::TlsPemSource::from_content(kSelfSignedCertPem);
    options.private_key = fiber::net::TlsPemSource::from_content(kSelfSignedKeyPem);
    auto credential = fiber::net::TlsCredential::create(options);
    if (!credential) {
        return nullptr;
    }
    return std::move(*credential);
}

fiber::http::HttpServerTlsOptions tls_options(fiber::net::TlsCredential &credential) {
    fiber::http::HttpServerTlsOptions options{};
    options.configure_callback = &fiber::net::configure_tls_with_credential;
    options.configure_ctx = &credential;
    return options;
}

TEST(Http2EndpointTest, ServesHttp2OverTlsAlpn) {
    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    auto credential = make_credential();
    ASSERT_NE(credential, nullptr);

    std::atomic<int> handled{0};
    auto running = start_server(group, Http2Endpoint::Options{
                                               .tls = tls_options(*credential),
                                               .handler =
                                                       [&handled](fiber::http::HttpExchange &exchange) {
                                                           handled.fetch_add(1, std::memory_order_relaxed);
                                                           return write_text(exchange, 200, "h2-ok");
                                                       },
                                       });
    ASSERT_NE(running.server, nullptr);

    ClientResult result = http2_round_trip(client_group, running.port);
    EXPECT_EQ(result.err, fiber::common::IoErr::None);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_EQ(result.body, "h2-ok");
    EXPECT_EQ(handled.load(), 1);

    running.stop_and_join();
    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

// allow_http1 keeps one endpoint serving both: same port, same handler, the
// peer's ALPN decides.
TEST(Http2EndpointTest, NegotiatesDownToHttp1WhenAllowed) {
    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    auto credential = make_credential();
    ASSERT_NE(credential, nullptr);

    std::atomic<int> handled{0};
    auto running = start_server(group, Http2Endpoint::Options{
                                               .tls = tls_options(*credential),
                                               .allow_http1 = true,
                                               .handler =
                                                       [&handled](fiber::http::HttpExchange &exchange) {
                                                           handled.fetch_add(1, std::memory_order_relaxed);
                                                           return write_text(exchange, 200, "shared");
                                                       },
                                       });
    ASSERT_NE(running.server, nullptr);

    ClientResult result = http2_round_trip(client_group, running.port);
    EXPECT_EQ(result.err, fiber::common::IoErr::None);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_EQ(result.body, "shared");
    EXPECT_EQ(handled.load(), 1);

    running.stop_and_join();
    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

// Plaintext cannot negotiate and Upgrade: h2c is not supported (D6), so
// allow_http1 picks the protocol outright.
TEST(Http2EndpointTest, PlaintextWithHttp1AllowedServesHttp1) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    auto running = start_server(group, Http2Endpoint::Options{
                                               .allow_http1 = true,
                                               .handler =
                                                       [](fiber::http::HttpExchange &exchange) {
                                                           return write_text(exchange, 200, "plain-h1");
                                                       },
                                       });
    ASSERT_NE(running.server, nullptr);

    int client = connect_to(running.port);
    ASSERT_GE(client, 0);
    const char *request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));
    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("plain-h1"), std::string::npos);

    running.stop_and_join();
    group.stop();
    group.join();
}

TEST(Http2EndpointTest, PlaintextWithoutHttp1ServesPriorKnowledgeH2c) {
    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    auto running = start_server(
            group,
            Http2Endpoint::Options{
                    .allow_http1 = false, // h2c prior knowledge
                    .handler = [](fiber::http::HttpExchange &exchange) { return write_text(exchange, 200, "h2c-ok"); },
            });
    ASSERT_NE(running.server, nullptr);

    std::promise<ClientResult> promise;
    auto future = promise.get_future();
    const std::uint16_t port = running.port;
    fiber::async::spawn(client_group.at(0), [&client_group, port, &promise]() -> DetachedTask {
        ClientResult result;
        fiber::http::Http2ClientConnection connection(client_group.at(0));
        // No TLS overload: cleartext prior knowledge.
        auto connected =
                co_await connection.connect(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port), 5s);
        if (!connected) {
            result.err = connected.error();
            promise.set_value(std::move(result));
            co_return;
        }

        auto run_state = std::make_shared<Http2RunState>();
        fiber::async::spawn(client_group.at(0),
                            [conn = &connection, state = run_state.get()]() { return run_connection(conn, state); });

        fiber::mem::BufPool pool;
        {
            fiber::http::ClientHttp2Exchange exchange(connection, pool);
            auto sent = co_await exchange.send_header(
                    {
                            .method = fiber::http::HttpMethod::Get,
                            .path = "/h2c",
                            .scheme = "http",
                            .authority = "localhost",
                    },
                    true);
            if (!sent) {
                result.err = sent.error();
            } else {
                auto header = co_await exchange.read_header();
                if (!header) {
                    result.err = header.error();
                } else {
                    result.status_code = (*header)->status_code;
                    auto body = co_await exchange.read_body(64);
                    if (body) {
                        result.body = chain_to_string(std::move(*body));
                    }
                }
            }
        }
        connection.shutdown();
        for (int i = 0; i < 2500 && !run_state->done.load(std::memory_order_acquire); ++i) {
            co_await fiber::async::sleep(2ms);
        }
        promise.set_value(std::move(result));
        co_return;
    });

    ClientResult result = future.get();
    EXPECT_EQ(result.err, fiber::common::IoErr::None);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_EQ(result.body, "h2c-ok");

    running.stop_and_join();
    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

// An h2 session with no streams in flight is the HTTP/2 equivalent of an idle
// keep-alive connection: GOAWAY closes it out immediately, so shutdown does not
// wait on a client that is merely still connected.
TEST(Http2EndpointTest, DrainClosesAnIdleSession) {
    fiber::event::EventLoopGroup group(2);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    auto credential = make_credential();
    ASSERT_NE(credential, nullptr);

    auto running = start_server(
            group,
            Http2Endpoint::Options{
                    .tls = tls_options(*credential),
                    .handler = [](fiber::http::HttpExchange &exchange) { return write_text(exchange, 200, "idle"); },
            });
    ASSERT_NE(running.server, nullptr);

    auto opened = std::make_shared<std::promise<void>>();
    auto opened_future = opened->get_future();
    std::promise<void> release;
    std::shared_future<void> release_future = release.get_future().share();

    std::promise<ClientResult> promise;
    auto client_future = promise.get_future();
    const std::uint16_t port = running.port;
    fiber::async::spawn(client_group.at(0), [&client_group, port, &promise, opened, release_future]() {
        return run_http2_client(&client_group.at(0), port, &promise, opened, release_future);
    });

    // Request done, client still connected and parked.
    ASSERT_EQ(opened_future.wait_for(10s), std::future_status::ready);

    running.server->stop();
    running.serve_done.get(); // completes without the client doing anything
    EXPECT_EQ(running.server->state(), Server::State::Stopped);

    release.set_value();
    ClientResult result = client_future.get();
    EXPECT_EQ(result.status_code, 200);
    EXPECT_EQ(result.body, "idle");

    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

// A stream still in flight holds the session open: GOAWAY stops new streams,
// the running one finishes, and only then does the server reach Stopped.
TEST(Http2EndpointTest, DrainWaitsForAnInFlightStream) {
    fiber::event::EventLoopGroup group(2);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    auto credential = make_credential();
    ASSERT_NE(credential, nullptr);

    std::promise<void> in_handler;
    auto in_handler_future = in_handler.get_future();
    std::atomic<bool> entered{false};
    std::atomic<bool> completed{false};

    auto running = start_server(
            group, Http2Endpoint::Options{
                           .tls = tls_options(*credential),
                           .handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
                               if (!entered.exchange(true)) {
                                   in_handler.set_value();
                               }
                               // Still running when stop() lands.
                               co_await fiber::async::sleep(300ms);
                               co_await write_text(exchange, 200, "late-but-complete");
                               completed.store(true);
                               co_return;
                           },
                   });
    ASSERT_NE(running.server, nullptr);

    std::promise<ClientResult> promise;
    auto client_future = promise.get_future();
    const std::uint16_t port = running.port;
    fiber::async::spawn(client_group.at(0), [&client_group, port, &promise]() {
        return run_http2_client(&client_group.at(0), port, &promise);
    });

    ASSERT_EQ(in_handler_future.wait_for(10s), std::future_status::ready);
    running.server->stop();

    std::this_thread::sleep_for(100ms);
    EXPECT_NE(running.server->state(), Server::State::Stopped); // the stream still holds it

    ClientResult result = client_future.get();
    running.serve_done.get();

    EXPECT_TRUE(completed.load());
    EXPECT_EQ(result.status_code, 200);
    EXPECT_EQ(result.body, "late-but-complete");
    EXPECT_EQ(running.server->state(), Server::State::Stopped);

    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

TEST(Http2EndpointTest, ConnectionsSpreadOverWorkerLoops) {
    fiber::event::EventLoopGroup accept_group(1);
    fiber::event::EventLoopGroup workers(2);
    fiber::event::EventLoopGroup client_group(1);
    accept_group.start();
    workers.start();
    client_group.start();

    auto credential = make_credential();
    ASSERT_NE(credential, nullptr);

    std::mutex loops_mu;
    std::set<fiber::event::EventLoop *> seen;
    auto running = start_server(accept_group,
                                Http2Endpoint::Options{
                                        .tls = tls_options(*credential),
                                        .handler =
                                                [&](fiber::http::HttpExchange &exchange) {
                                                    {
                                                        std::lock_guard guard(loops_mu);
                                                        seen.insert(&fiber::event::EventLoop::current());
                                                    }
                                                    return write_text(exchange, 200, "ok");
                                                },
                                },
                                &workers);
    ASSERT_NE(running.server, nullptr);
    EXPECT_EQ(running.server->worker_count(), 2u);

    for (int i = 0; i < 2; ++i) {
        ClientResult result = http2_round_trip(client_group, running.port);
        EXPECT_EQ(result.err, fiber::common::IoErr::None);
        EXPECT_EQ(result.status_code, 200);
    }

    {
        std::lock_guard guard(loops_mu);
        EXPECT_EQ(seen.size(), 2u);
        for (fiber::event::EventLoop *loop: seen) {
            EXPECT_NE(loop, &accept_group.at(0));
        }
    }

    running.stop_and_join();
    accept_group.stop();
    accept_group.join();
    workers.stop();
    workers.join();
    client_group.stop();
    client_group.join();
}

} // namespace
