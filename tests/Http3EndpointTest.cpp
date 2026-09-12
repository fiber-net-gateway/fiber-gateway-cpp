#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/TaskSelect.h>
#include <fiber/async/WhenAny.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/ClientHttp3Exchange.h>
#include <fiber/http/Http3Client.h>
#include <fiber/http/HttpBodyPipe.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http/HttpResponseWriter.h>
#include <fiber/http/Server.h>
#include <fiber/http/endpoint/Http1Endpoint.h>
#include <fiber/http/endpoint/Http3Endpoint.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/net/TrustStore.h>
#include <fiber/quic/QuicUdpEndpoint.h>

#include "QuicTestTlsCertificate.h"

namespace {

using fiber::async::DetachedTask;
using fiber::http::Http1Endpoint;
using fiber::http::Http3Endpoint;
using fiber::http::Server;
using namespace std::chrono_literals;

fiber::async::Task<void> write_text(fiber::http::HttpExchange &exchange, int status, std::string_view body) {
    fiber::http::HttpHeaders headers(exchange.pool());
    const std::string content_length = std::to_string(body.size());
    headers.set("content-length", content_length);
    auto sent = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = status,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::ContentLength(body.size()),
            .end_stream = body.empty(),
    });
    if (!sent || body.empty()) {
        co_return;
    }
    (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true, 2s);
    co_return;
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

struct ClientResult {
    fiber::common::IoErr error = fiber::common::IoErr::None;
    bool connected = false;
    int status = 0;
    std::string body{};
    // A second request attempted after an idle wait, to show whether the server
    // kept the session.
    fiber::common::IoErr second_request_error = fiber::common::IoErr::None;
    int second_status = 0;
};

// One HTTP/3 request over a fresh QUIC connection. `hold` (when valid) keeps
// the connection open after the response so a test can drain the server while
// a client is still attached.
DetachedTask run_http3_client(fiber::event::EventLoop *loop, fiber::net::SocketAddress server_addr,
                              std::string cert_path, std::promise<ClientResult> *promise,
                              std::shared_ptr<std::promise<void>> done_first = {}, std::shared_future<void> hold = {},
                              std::uint64_t request_stream_window = 0) {
    ClientResult result{};
    fiber::quic::QuicUdpEndpoint endpoint(*loop);
    fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
    endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    auto endpoint_ready = endpoint.init(endpoint_options);
    if (!endpoint_ready) {
        result.error = endpoint_ready.error();
        promise->set_value(std::move(result));
        co_return;
    }

    auto trust_store = fiber::net::TrustStore::create(fiber::net::TrustStoreOptions::from_file(cert_path));
    if (!trust_store) {
        result.error = trust_store.error();
        co_await endpoint.shutdown();
        promise->set_value(std::move(result));
        co_return;
    }

    fiber::http::Http3Client::Options client_options{};
    client_options.tls.trust_store = trust_store->get();
    client_options.tls.verify_peer = true;
    fiber::http::Http3Client client(endpoint, std::move(client_options));

    auto started = endpoint.start();
    if (!started) {
        result.error = started.error();
        co_await endpoint.shutdown();
        promise->set_value(std::move(result));
        co_return;
    }
    auto initialized = client.init();
    if (!initialized) {
        result.error = initialized.error();
        co_await endpoint.shutdown();
        promise->set_value(std::move(result));
        co_return;
    }

    fiber::http::Http3ClientConnectOptions connect_options{};
    connect_options.remote_addr = server_addr;
    connect_options.server_name = "localhost";
    connect_options.handshake_timeout = 3s;
    if (request_stream_window != 0) {
        connect_options.transport.initial_max_stream_data_bidi_local = request_stream_window;
    }
    auto connected = co_await client.connect(std::move(connect_options));
    if (!connected) {
        result.error = connected.error().io_error;
        co_await endpoint.shutdown();
        promise->set_value(std::move(result));
        co_return;
    }
    result.connected = true;

    {
        fiber::mem::BufPool pool;
        fiber::http::ClientHttp3Exchange exchange = connected->open_exchange(pool);
        auto sent = co_await exchange.send_request_header(
                {
                        .method = fiber::http::HttpMethod::Get,
                        .scheme = "https",
                        .authority = "localhost",
                        .path = "/h3",
                },
                true, 3s);
        if (!sent) {
            result.error = sent.error();
        } else {
            auto head = co_await exchange.read_header(5s);
            if (!head || *head == nullptr) {
                result.error = head ? fiber::common::IoErr::Invalid : head.error();
            } else {
                result.status = (*head)->status_code;
                bool complete = false;
                while (result.error == fiber::common::IoErr::None && !complete) {
                    auto chunk = co_await exchange.read_body(64 * 1024, 5s);
                    if (!chunk) {
                        result.error = chunk.error();
                        break;
                    }
                    complete = chunk->complete();
                    result.body.append(chain_to_string(std::move(*chunk)));
                }
            }
        }
    }

    if (done_first) {
        done_first->set_value();
    }
    if (hold.valid()) {
        while (hold.wait_for(0ms) != std::future_status::ready) {
            co_await fiber::async::sleep(2ms);
        }
    }

    connected->shutdown(fiber::http::Http3ErrorCode::NoError);
    *connected = fiber::http::Http3ClientConnection{};
    // Let the CONNECTION_CLOSE actually reach the wire before tearing the
    // endpoint down; otherwise the server sees a peer that simply vanished.
    co_await fiber::async::sleep(50ms);
    co_await endpoint.shutdown();
    promise->set_value(std::move(result));
    co_return;
}

// Client that completes one request, waits with the connection open but idle,
// then tries a second request. Whether that one lands is the whole question:
// the QUIC idle timeout is 30s away and the link is healthy, so nothing but the
// HTTP/3 layer can end this session.
//
// Deliberately not asserting on the GOAWAY: with no request in flight the
// server writes it and closes in the same turn, so which of the two the client
// parses first is a race.
DetachedTask run_http3_client_idle_reclaim(fiber::event::EventLoop *loop, fiber::net::SocketAddress server_addr,
                                           std::string cert_path, std::chrono::milliseconds wait,
                                           std::promise<ClientResult> *promise, bool send_first_request = true) {
    ClientResult result{};
    fiber::quic::QuicUdpEndpoint endpoint(*loop);
    fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
    endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    auto endpoint_ready = endpoint.init(endpoint_options);
    if (!endpoint_ready) {
        result.error = endpoint_ready.error();
        promise->set_value(std::move(result));
        co_return;
    }

    auto trust_store = fiber::net::TrustStore::create(fiber::net::TrustStoreOptions::from_file(cert_path));
    if (!trust_store) {
        result.error = trust_store.error();
        co_await endpoint.shutdown();
        promise->set_value(std::move(result));
        co_return;
    }

    fiber::http::Http3Client::Options client_options{};
    client_options.tls.trust_store = trust_store->get();
    client_options.tls.verify_peer = true;
    fiber::http::Http3Client client(endpoint, std::move(client_options));

    auto started = endpoint.start();
    if (!started) {
        result.error = started.error();
        co_await endpoint.shutdown();
        promise->set_value(std::move(result));
        co_return;
    }
    auto initialized = client.init();
    if (!initialized) {
        result.error = initialized.error();
        co_await endpoint.shutdown();
        promise->set_value(std::move(result));
        co_return;
    }

    fiber::http::Http3ClientConnectOptions connect_options{};
    connect_options.remote_addr = server_addr;
    connect_options.server_name = "localhost";
    connect_options.handshake_timeout = 3s;
    auto connected = co_await client.connect(std::move(connect_options));
    if (!connected) {
        result.error = connected.error().io_error;
        co_await endpoint.shutdown();
        promise->set_value(std::move(result));
        co_return;
    }
    result.connected = true;

    if (send_first_request) {
        fiber::mem::BufPool pool;
        fiber::http::ClientHttp3Exchange exchange = connected->open_exchange(pool);
        auto sent = co_await exchange.send_request_header(
                {
                        .method = fiber::http::HttpMethod::Get,
                        .scheme = "https",
                        .authority = "localhost",
                        .path = "/h3",
                },
                true, 3s);
        if (!sent) {
            result.error = sent.error();
        } else {
            auto head = co_await exchange.read_header(5s);
            if (!head || *head == nullptr) {
                result.error = head ? fiber::common::IoErr::Invalid : head.error();
            } else {
                result.status = (*head)->status_code;
                bool complete = false;
                while (result.error == fiber::common::IoErr::None && !complete) {
                    auto chunk = co_await exchange.read_body(64 * 1024, 5s);
                    if (!chunk) {
                        result.error = chunk.error();
                        break;
                    }
                    complete = chunk->complete();
                    result.body.append(chain_to_string(std::move(*chunk)));
                }
            }
        }
    }

    co_await fiber::async::sleep(wait);

    {
        fiber::mem::BufPool pool;
        fiber::http::ClientHttp3Exchange exchange = connected->open_exchange(pool);
        auto sent = co_await exchange.send_request_header(
                {
                        .method = fiber::http::HttpMethod::Get,
                        .scheme = "https",
                        .authority = "localhost",
                        .path = "/h3",
                },
                true, 2s);
        if (!sent) {
            result.second_request_error = sent.error();
        } else {
            auto head = co_await exchange.read_header(3s);
            if (!head || *head == nullptr) {
                result.second_request_error = head ? fiber::common::IoErr::Invalid : head.error();
            } else {
                result.second_status = (*head)->status_code;
            }
        }
    }

    connected->shutdown(fiber::http::Http3ErrorCode::NoError);
    *connected = fiber::http::Http3ClientConnection{};
    co_await fiber::async::sleep(50ms);
    co_await endpoint.shutdown();
    promise->set_value(std::move(result));
    co_return;
}

// Client that reads the response head and then aborts the stream without
// draining the body (RESET_STREAM + STOP_SENDING, the browser-abort shape).
// Combined with a tiny initial stream window this leaves the server's body
// writer suspended on flow control when the send-abort arrives.
DetachedTask run_http3_client_close_after_header(fiber::event::EventLoop *loop, fiber::net::SocketAddress server_addr,
                                                 std::string cert_path, std::promise<ClientResult> *promise,
                                                 std::uint64_t request_stream_window) {
    ClientResult result{};
    fiber::quic::QuicUdpEndpoint endpoint(*loop);
    fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
    endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    auto endpoint_ready = endpoint.init(endpoint_options);
    if (!endpoint_ready) {
        result.error = endpoint_ready.error();
        promise->set_value(std::move(result));
        co_return;
    }

    auto trust_store = fiber::net::TrustStore::create(fiber::net::TrustStoreOptions::from_file(cert_path));
    if (!trust_store) {
        result.error = trust_store.error();
        co_await endpoint.shutdown();
        promise->set_value(std::move(result));
        co_return;
    }

    fiber::http::Http3Client::Options client_options{};
    client_options.tls.trust_store = trust_store->get();
    client_options.tls.verify_peer = true;
    fiber::http::Http3Client client(endpoint, std::move(client_options));

    auto started = endpoint.start();
    if (!started) {
        result.error = started.error();
        co_await endpoint.shutdown();
        promise->set_value(std::move(result));
        co_return;
    }
    auto initialized = client.init();
    if (!initialized) {
        result.error = initialized.error();
        co_await endpoint.shutdown();
        promise->set_value(std::move(result));
        co_return;
    }

    fiber::http::Http3ClientConnectOptions connect_options{};
    connect_options.remote_addr = server_addr;
    connect_options.server_name = "localhost";
    connect_options.handshake_timeout = 3s;
    connect_options.transport.initial_max_stream_data_bidi_local = request_stream_window;
    auto connected = co_await client.connect(std::move(connect_options));
    if (!connected) {
        result.error = connected.error().io_error;
        co_await endpoint.shutdown();
        promise->set_value(std::move(result));
        co_return;
    }
    result.connected = true;

    {
        fiber::mem::BufPool pool;
        fiber::http::ClientHttp3Exchange exchange = connected->open_exchange(pool);
        auto sent = co_await exchange.send_request_header(
                {
                        .method = fiber::http::HttpMethod::Get,
                        .scheme = "https",
                        .authority = "localhost",
                        .path = "/h3",
                },
                true, 3s);
        if (!sent) {
            result.error = sent.error();
        } else {
            auto head = co_await exchange.read_header(5s);
            if (!head || *head == nullptr) {
                result.error = head ? fiber::common::IoErr::Invalid : head.error();
            } else {
                result.status = (*head)->status_code;
                // Give the server time to push the body into the peer's grant
                // ceiling and suspend the writer on flow control, then abort
                // the response stream (RESET_STREAM + STOP_SENDING). The
                // server observes send-aborted while its writer is suspended:
                // the production browser-abort shape, where the
                // channel-closed resume is queued ahead of the suspended
                // writer's own completion.
                co_await fiber::async::sleep(500ms);
                (void) exchange.abort(fiber::common::IoErr::Canceled);
            }
        }
    }

    // Keep the connection alive long enough for the abort frames to reach the
    // server and its teardown to run, then vanish.
    co_await fiber::async::sleep(50ms);
    *connected = fiber::http::Http3ClientConnection{};
    co_await endpoint.shutdown();
    promise->set_value(std::move(result));
    co_return;
}

struct RunningServer {
    std::unique_ptr<Server> server{};
    Http3Endpoint *endpoint = nullptr;
    Http1Endpoint *tcp_endpoint = nullptr;
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

void spawn_serve(RunningServer &running, fiber::event::EventLoop &loop) {
    auto done = std::make_shared<std::promise<void>>();
    running.serve_done = done->get_future();
    Server *server = running.server.get();
    fiber::async::spawn(loop, [server, done]() -> DetachedTask {
        co_await server->serve();
        done->set_value();
        co_return;
    });
}

fiber::http::HttpServerTlsOptions tls_options(fiber::net::TlsCredential &credential) {
    fiber::http::HttpServerTlsOptions options{};
    options.configure_callback = &fiber::net::configure_tls_with_credential;
    options.configure_ctx = &credential;
    return options;
}

struct TestCredential {
    fiber::test::QuicTestTlsFile cert{"h3-endpoint-cert", fiber::test::kQuicTestCertificatePem};
    fiber::test::QuicTestTlsFile key{"h3-endpoint-key", fiber::test::kQuicTestPrivateKeyPem};
    std::unique_ptr<fiber::net::TlsCredential> credential{};

    bool init() {
        if (!cert.valid() || !key.valid()) {
            return false;
        }
        fiber::net::TlsCredentialOptions options{};
        options.certificate_chain = fiber::net::TlsPemSource::from_file(cert.path());
        options.private_key = fiber::net::TlsPemSource::from_file(key.path());
        auto created = fiber::net::TlsCredential::create(options);
        if (!created) {
            return false;
        }
        credential = std::move(*created);
        return true;
    }
};

TEST(Http3EndpointTest, ServesHttp3Requests) {
    TestCredential tls;
    ASSERT_TRUE(tls.init());

    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    std::atomic<int> handled{0};
    RunningServer running;
    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
            .tls = tls_options(*tls.credential),
            .handler =
                    [&handled](fiber::http::HttpExchange &exchange) {
                        handled.fetch_add(1, std::memory_order_relaxed);
                        return write_text(exchange, 200, "h3-ok");
                    },
    });
    ASSERT_NE(running.endpoint, nullptr);
    ASSERT_TRUE(running.server->start().has_value());
    spawn_serve(running, group.at(0));

    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
    ASSERT_NE(server_addr.port(), 0);

    std::promise<ClientResult> promise;
    auto future = promise.get_future();
    const std::string cert_path = tls.cert.path();
    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
        return run_http3_client(&client_group.at(0), server_addr, cert_path, &promise);
    });

    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
    ClientResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.status, 200);
    EXPECT_EQ(result.body, "h3-ok");
    EXPECT_EQ(handled.load(), 1);

    running.stop_and_join();
    EXPECT_EQ(running.server->state(), Server::State::Stopped);

    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

// A proxied streamed response (no Content-Length) sends the body through
// HttpExchange::write with the completion marker riding the final chain, the
// way http::pipe_http_body drives its sink. The exchange must observe the
// marker consumed and record the response as completed.
// A session with no request running is the server's to reclaim, and only the
// HTTP/3 layer can tell: the four control streams mean QUIC's own stream count
// never reaches zero, and the transport is perfectly healthy besides.
//
// Paired with IdleConnectionTimeoutDisabledKeepsTheSession below, which runs
// the identical client against a server that has the timeout off. Only the
// setting differs, so the wait itself cannot be what ends the session.
TEST(Http3EndpointTest, IdleConnectionTimeoutRetiresASessionWithNoRequests) {
    TestCredential tls;
    ASSERT_TRUE(tls.init());

    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    std::atomic<int> handled{0};
    fiber::http::Http3ServerOptions http3_options{};
    http3_options.idle_connection_timeout = 300ms;

    RunningServer running;
    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
            .tls = tls_options(*tls.credential),
            .http3 = http3_options,
            .handler =
                    [&handled](fiber::http::HttpExchange &exchange) {
                        handled.fetch_add(1, std::memory_order_relaxed);
                        return write_text(exchange, 200, "h3-ok");
                    },
    });
    ASSERT_NE(running.endpoint, nullptr);
    ASSERT_TRUE(running.server->start().has_value());
    spawn_serve(running, group.at(0));

    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
    ASSERT_NE(server_addr.port(), 0);

    std::promise<ClientResult> promise;
    auto future = promise.get_future();
    const std::string cert_path = tls.cert.path();
    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
        return run_http3_client_idle_reclaim(&client_group.at(0), server_addr, cert_path, 900ms, &promise);
    });

    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
    ClientResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.status, 200);
    EXPECT_EQ(result.body, "h3-ok");
    EXPECT_EQ(handled.load(), 1);

    // Retired while idle: the second request has nowhere to go.
    EXPECT_NE(result.second_request_error, fiber::common::IoErr::None);
    EXPECT_EQ(handled.load(), 1);

    running.stop_and_join();
    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

TEST(Http3EndpointTest, IdleConnectionTimeoutRetiresASessionThatNeverSentARequest) {
    TestCredential tls;
    ASSERT_TRUE(tls.init());

    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    std::atomic<int> handled{0};
    fiber::http::Http3ServerOptions http3_options{};
    http3_options.idle_connection_timeout = 300ms;

    RunningServer running;
    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
            .tls = tls_options(*tls.credential),
            .http3 = http3_options,
            .handler =
                    [&handled](fiber::http::HttpExchange &exchange) {
                        handled.fetch_add(1, std::memory_order_relaxed);
                        return write_text(exchange, 200, "h3-ok");
                    },
    });
    ASSERT_NE(running.endpoint, nullptr);
    ASSERT_TRUE(running.server->start().has_value());
    spawn_serve(running, group.at(0));

    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
    ASSERT_NE(server_addr.port(), 0);

    std::promise<ClientResult> promise;
    auto future = promise.get_future();
    const std::string cert_path = tls.cert.path();
    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
        return run_http3_client_idle_reclaim(&client_group.at(0), server_addr, cert_path, 900ms, &promise, false);
    });

    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
    ClientResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_TRUE(result.connected);

    // Retired while idle: the second request has nowhere to go.
    EXPECT_NE(result.second_request_error, fiber::common::IoErr::None);
    EXPECT_EQ(handled.load(), 0);

    running.stop_and_join();
    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

TEST(Http3EndpointTest, IdleConnectionTimeoutPreservesLongRequestsAndRestartsAfterCompletion) {
    TestCredential tls;
    ASSERT_TRUE(tls.init());

    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    std::atomic<int> handled{0};
    fiber::http::Http3ServerOptions http3_options{};
    http3_options.idle_connection_timeout = 300ms;

    RunningServer running;
    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
            .tls = tls_options(*tls.credential),
            .http3 = http3_options,
            .handler = [&handled](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
                handled.fetch_add(1, std::memory_order_relaxed);
                co_await fiber::async::sleep(600ms);
                co_await write_text(exchange, 200, "h3-ok");
            },
    });
    ASSERT_NE(running.endpoint, nullptr);
    ASSERT_TRUE(running.server->start().has_value());
    spawn_serve(running, group.at(0));

    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
    ASSERT_NE(server_addr.port(), 0);

    std::promise<ClientResult> promise;
    auto future = promise.get_future();
    const std::string cert_path = tls.cert.path();
    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
        return run_http3_client_idle_reclaim(&client_group.at(0), server_addr, cert_path, 100ms, &promise);
    });

    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
    ClientResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.status, 200);
    EXPECT_EQ(result.body, "h3-ok");
    EXPECT_EQ(handled.load(), 2);

    // Each response takes longer than the idle budget. After the first
    // completes, a short idle period must still leave a fresh request budget.
    EXPECT_EQ(result.second_request_error, fiber::common::IoErr::None);
    EXPECT_EQ(result.second_status, 200);
    EXPECT_EQ(handled.load(), 2);

    running.stop_and_join();
    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

TEST(Http3EndpointTest, IdleConnectionTimeoutDisabledKeepsTheSession) {
    TestCredential tls;
    ASSERT_TRUE(tls.init());

    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    std::atomic<int> handled{0};
    RunningServer running;
    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
    // Explicit zero: idle_connection_timeout defaults to 70s now, but this
    // case pins the disabled behavior.
    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
            .tls = tls_options(*tls.credential),
            .http3 = {.idle_connection_timeout = 0ms},
            .handler =
                    [&handled](fiber::http::HttpExchange &exchange) {
                        handled.fetch_add(1, std::memory_order_relaxed);
                        return write_text(exchange, 200, "h3-ok");
                    },
    });
    ASSERT_NE(running.endpoint, nullptr);
    ASSERT_TRUE(running.server->start().has_value());
    spawn_serve(running, group.at(0));

    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
    ASSERT_NE(server_addr.port(), 0);

    std::promise<ClientResult> promise;
    auto future = promise.get_future();
    const std::string cert_path = tls.cert.path();
    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
        return run_http3_client_idle_reclaim(&client_group.at(0), server_addr, cert_path, 900ms, &promise);
    });

    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
    ClientResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.status, 200);

    // Same wait, same client: the session survives because nothing retires it.
    EXPECT_EQ(result.second_request_error, fiber::common::IoErr::None);
    EXPECT_EQ(result.second_status, 200);
    EXPECT_EQ(handled.load(), 2);

    running.stop_and_join();
    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

TEST(Http3EndpointTest, StreamedAutoBodyViaChainWriteCompletes) {
    TestCredential tls;
    ASSERT_TRUE(tls.init());

    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    std::atomic<fiber::common::IoErr> header_error{fiber::common::IoErr::None};
    std::atomic<fiber::common::IoErr> write_error{fiber::common::IoErr::None};
    std::atomic<bool> write_ok{false};
    std::atomic<bool> stats_completed{false};
    std::atomic<fiber::common::IoErr> stats_terminal{fiber::common::IoErr::None};

    RunningServer running;
    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
            .tls = tls_options(*tls.credential),
            .handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
                fiber::http::HttpHeaders headers(exchange.pool());
                headers.set("content-type", "application/json");
                auto sent = co_await exchange.send_header(
                        {
                                .kind = fiber::http::OutgoingHeaderKind::Final,
                                .status_code = 200,
                                .headers = &headers,
                                .body = fiber::http::HttpBodySpec::Auto(),
                                .connection_mode = fiber::http::ResponseConnectionMode::Auto,
                                .end_stream = false,
                        },
                        2s);
                header_error.store(sent ? fiber::common::IoErr::None : sent.error());
                if (!sent) {
                    co_return;
                }

                fiber::mem::IoBufChain chain(fiber::event::EventLoop::current().io_buf_node_pool());
                fiber::mem::IoBuf body = fiber::mem::IoBuf::allocate(136);
                body.commit(136);
                chain.append(std::move(body));
                chain.mark_complete();

                auto written = co_await exchange.write(chain, 2s);
                write_ok.store(written.has_value());
                write_error.store(written ? fiber::common::IoErr::None : written.error());
                stats_completed.store(exchange.response_stats().completed);
                stats_terminal.store(exchange.response_stats().terminal_error);
                co_return;
            },
    });
    ASSERT_NE(running.endpoint, nullptr);
    ASSERT_TRUE(running.server->start().has_value());
    spawn_serve(running, group.at(0));

    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
    ASSERT_NE(server_addr.port(), 0);

    std::promise<ClientResult> promise;
    auto future = promise.get_future();
    const std::string cert_path = tls.cert.path();
    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
        return run_http3_client(&client_group.at(0), server_addr, cert_path, &promise);
    });

    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
    ClientResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.status, 200);
    EXPECT_EQ(result.body.size(), 136u);

    EXPECT_EQ(header_error.load(), fiber::common::IoErr::None);
    EXPECT_TRUE(write_ok.load());
    EXPECT_EQ(write_error.load(), fiber::common::IoErr::None);
    EXPECT_TRUE(stats_completed.load());
    EXPECT_EQ(stats_terminal.load(), fiber::common::IoErr::None);

    running.stop_and_join();
    EXPECT_EQ(running.server->state(), Server::State::Stopped);

    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

TEST(Http3EndpointTest, StreamedAutoBodyWithTinyRequestStreamWindow) {
    TestCredential tls;
    ASSERT_TRUE(tls.init());

    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    std::atomic<fiber::common::IoErr> pipe_error{fiber::common::IoErr::None};
    std::atomic<bool> pipe_ok{false};
    std::atomic<bool> stats_completed{false};
    std::atomic<fiber::common::IoErr> stats_terminal{fiber::common::IoErr::None};

    RunningServer running;
    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
            .tls = tls_options(*tls.credential),
            .handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
                fiber::http::HttpHeaders headers(exchange.pool());
                headers.set("content-type", "application/json");
                auto sent = co_await exchange.send_header(
                        {
                                .kind = fiber::http::OutgoingHeaderKind::Final,
                                .status_code = 200,
                                .headers = &headers,
                                .body = fiber::http::HttpBodySpec::Auto(),
                                .connection_mode = fiber::http::ResponseConnectionMode::Auto,
                                .end_stream = false,
                        },
                        2s);
                if (!sent) {
                    co_return;
                }

                struct FixedSource {
                    fiber::mem::IoBufChain reads[2];
                    unsigned served = 0;
                    fiber::common::IoErr abort_error{fiber::common::IoErr::None};

                    fiber::async::Task<fiber::common::IoResult<fiber::mem::IoBufChain>>
                    read_body(std::size_t max_bytes, std::chrono::milliseconds) noexcept {
                        if (served >= 2) {
                            co_return std::unexpected(fiber::common::IoErr::Invalid);
                        }
                        if (reads[served].readable_bytes() > max_bytes) {
                            co_return std::unexpected(fiber::common::IoErr::MessageTooLarge);
                        }
                        co_return std::move(reads[served++]);
                    }
                    fiber::common::IoResult<void> abort(fiber::common::IoErr reason) noexcept {
                        abort_error = reason;
                        return {};
                    }
                };
                FixedSource source{fiber::mem::IoBufChain(fiber::event::EventLoop::current().io_buf_node_pool()),
                                   fiber::mem::IoBufChain(fiber::event::EventLoop::current().io_buf_node_pool())};
                fiber::mem::IoBuf body = fiber::mem::IoBuf::allocate(136);
                body.commit(136);
                source.reads[0].append(std::move(body));
                source.reads[1].mark_complete();

                fiber::http::HttpResponseWriter writer = fiber::http::make_http_response_writer(exchange);
                const fiber::http::HttpBodyPipeOptions pipe_options{
                        .buffer_size = 64 * 1024,
                        .low_water = std::min<std::size_t>(64 * 1024, fiber::http::kDefaultBodyPipeLowWater),
                        .read_timeout = std::chrono::milliseconds::max(),
                        .write_timeout = 2s,
                };
                auto piped = co_await fiber::http::pipe_http_body(fiber::http::make_http_body_pipe_reader(source),
                                                                  fiber::http::make_http_body_pipe_writer(writer),
                                                                  fiber::event::EventLoop::current().io_buf_node_pool(),
                                                                  pipe_options);
                pipe_ok.store(piped.has_value());
                pipe_error.store(piped ? fiber::common::IoErr::None : piped.error().code);
                stats_completed.store(exchange.response_stats().completed);
                stats_terminal.store(exchange.response_stats().terminal_error);
                co_return;
            },
    });
    ASSERT_NE(running.endpoint, nullptr);
    ASSERT_TRUE(running.server->start().has_value());
    spawn_serve(running, group.at(0));

    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
    ASSERT_NE(server_addr.port(), 0);

    std::promise<ClientResult> promise;
    auto future = promise.get_future();
    const std::string cert_path = tls.cert.path();
    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
        return run_http3_client(&client_group.at(0), server_addr, cert_path, &promise, {}, {}, 64);
    });

    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
    ClientResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.status, 200);
    EXPECT_EQ(result.body.size(), 136u);

    EXPECT_TRUE(pipe_ok.load());
    EXPECT_EQ(pipe_error.load(), fiber::common::IoErr::None);
    EXPECT_TRUE(stats_completed.load());
    EXPECT_EQ(stats_terminal.load(), fiber::common::IoErr::None);

    running.stop_and_join();
    EXPECT_EQ(running.server->state(), Server::State::Stopped);

    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

// A handler whose body writer is hard-destroyed while suspended on flow
// control (the production shape: when_any(response-channel-closed, proxy
// task) discards the loser after the peer vanished mid-response). The QUIC
// write awaiter must retract every loop registration it armed, including a
// queued resume, or the event loop later pops and calls into freed memory
// (heap-use-after-free in MpscQueue::try_pop_all, wild jump in release).
TEST(Http3EndpointTest, DestroyedSuspendedBodyWriterKeepsLoopIntact) {
    TestCredential tls;
    ASSERT_TRUE(tls.init());

    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    std::atomic<bool> writer_finished{false};
    std::atomic<bool> closed_path_taken{false};

    RunningServer running;
    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
            .tls = tls_options(*tls.credential),
            .handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
                fiber::http::HttpHeaders headers(exchange.pool());
                headers.set("content-type", "application/json");
                auto sent = co_await exchange.send_header(
                        {
                                .kind = fiber::http::OutgoingHeaderKind::Final,
                                .status_code = 200,
                                .headers = &headers,
                                .body = fiber::http::HttpBodySpec::Auto(),
                                .connection_mode = fiber::http::ResponseConnectionMode::Auto,
                                .end_stream = false,
                        },
                        2s);
                if (!sent) {
                    co_return;
                }

                auto write_body = [](fiber::http::HttpExchange &exchange,
                                     std::atomic<bool> *finished) -> fiber::async::Task<void> {
                    struct EndlessSource {
                        fiber::mem::IoBufNodePool *pool = nullptr;
                        std::size_t remaining = 600 * 1024;
                        fiber::common::IoResult<void> abort(fiber::common::IoErr) noexcept { return {}; }
                        fiber::async::Task<fiber::common::IoResult<fiber::mem::IoBufChain>>
                        read_body(std::size_t max_bytes, std::chrono::milliseconds) noexcept {
                            fiber::mem::IoBufChain chunk(*pool);
                            if (remaining == 0) {
                                chunk.mark_complete();
                                co_return chunk;
                            }
                            const std::size_t len = std::min(max_bytes, remaining);
                            fiber::mem::IoBuf body = fiber::mem::IoBuf::allocate(len);
                            body.commit(len);
                            chunk.append(std::move(body));
                            remaining -= len;
                            co_return chunk;
                        }
                    };
                    // ~600 KiB total: far beyond the peer's grant ceiling
                    // (consumed + 64 KiB stream recv buffer), so the
                    // writer genuinely suspends inside QuicStream::write
                    // before the client aborts.
                    EndlessSource source{&fiber::event::EventLoop::current().io_buf_node_pool(), 600 * 1024};

                    fiber::http::HttpResponseWriter writer = fiber::http::make_http_response_writer(exchange);
                    const fiber::http::HttpBodyPipeOptions pipe_options{
                            .buffer_size = 64 * 1024,
                            .low_water = fiber::http::kUnbufferedBodyPipeLowWater,
                            .read_timeout = std::chrono::milliseconds::max(),
                            .write_timeout = 5s,
                    };
                    auto piped = co_await fiber::http::pipe_http_body(
                            fiber::http::make_http_body_pipe_reader(source),
                            fiber::http::make_http_body_pipe_writer(writer),
                            fiber::event::EventLoop::current().io_buf_node_pool(), pipe_options);
                    (void) piped;
                    finished->store(true);
                };

                auto completed = co_await fiber::async::when_any(
                        [&exchange]() { return exchange.wait_response_channel_closed(); },
                        [&exchange, &write_body, &writer_finished]() {
                            return write_body(exchange, &writer_finished).select();
                        });
                closed_path_taken.store(completed.is<0>());
                co_return;
            },
    });
    ASSERT_NE(running.endpoint, nullptr);
    ASSERT_TRUE(running.server->start().has_value());
    spawn_serve(running, group.at(0));

    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
    ASSERT_NE(server_addr.port(), 0);

    std::promise<ClientResult> promise;
    auto future = promise.get_future();
    const std::string cert_path = tls.cert.path();
    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
        return run_http3_client_close_after_header(&client_group.at(0), server_addr, cert_path, &promise, 64);
    });

    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
    ClientResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.status, 200);

    // The send-abort races the suspended writer's BrokenPipe completion
    // through the loop's queues; either side may win depending on the drain
    // interleaving. What must not happen is the loop touching freed memory
    // when the when_any winner hard-destroys the loser while its write resume
    // is still queued: the follow-up request below checks loop health (and,
    // under ASan, the allocator checks the rest — the churn repro that found
    // this defect reproduces it deterministically enough there).
    EXPECT_TRUE(closed_path_taken.load() || writer_finished.load());

    // The loop must still be healthy after the destroyed writer: a follow-up
    // request through the same server completes normally.
    {
        std::promise<ClientResult> followup;
        auto followup_future = followup.get_future();
        fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &followup]() {
            return run_http3_client(&client_group.at(0), server_addr, cert_path, &followup);
        });
        ASSERT_EQ(followup_future.wait_for(15s), std::future_status::ready);
        ClientResult followup_result = followup_future.get();
        EXPECT_EQ(followup_result.error, fiber::common::IoErr::None);
        EXPECT_EQ(followup_result.status, 200);
    }

    running.stop_and_join();
    EXPECT_EQ(running.server->state(), Server::State::Stopped);

    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

// Closing the UDP socket the moment shutdown begins would
// killed every session outright. Draining now keeps the socket open until the
// live sessions finish.
TEST(Http3EndpointTest, DrainLetsAnInFlightRequestFinish) {
    TestCredential tls;
    ASSERT_TRUE(tls.init());

    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    std::promise<void> in_handler;
    auto in_handler_future = in_handler.get_future();
    std::atomic<bool> entered{false};
    std::atomic<bool> completed{false};

    RunningServer running;
    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
            .tls = tls_options(*tls.credential),
            .handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
                if (!entered.exchange(true)) {
                    in_handler.set_value();
                }
                co_await fiber::async::sleep(300ms); // still running when stop() lands
                co_await write_text(exchange, 200, "late-but-complete");
                completed.store(true);
                co_return;
            },
    });
    ASSERT_NE(running.endpoint, nullptr);
    ASSERT_TRUE(running.server->start().has_value());
    spawn_serve(running, group.at(0));

    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
    std::promise<ClientResult> promise;
    auto future = promise.get_future();
    const std::string cert_path = tls.cert.path();
    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
        return run_http3_client(&client_group.at(0), server_addr, cert_path, &promise);
    });

    ASSERT_EQ(in_handler_future.wait_for(15s), std::future_status::ready);
    running.server->stop();

    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
    ClientResult result = future.get();
    running.serve_done.get();

    EXPECT_TRUE(completed.load());
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.status, 200);
    EXPECT_EQ(result.body, "late-but-complete");
    EXPECT_EQ(running.server->state(), Server::State::Stopped);

    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

TEST(Http3EndpointTest, StoppedEndpointRefusesNewConnections) {
    TestCredential tls;
    ASSERT_TRUE(tls.init());

    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    RunningServer running;
    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
            .tls = tls_options(*tls.credential),
            .handler = [](fiber::http::HttpExchange &exchange) { return write_text(exchange, 200, "ok"); },
    });
    ASSERT_NE(running.endpoint, nullptr);
    ASSERT_TRUE(running.server->start().has_value());
    spawn_serve(running, group.at(0));

    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
    const std::string cert_path = tls.cert.path();
    running.stop_and_join();
    EXPECT_EQ(running.server->state(), Server::State::Stopped);

    // Socket is gone with the last session, so a fresh handshake cannot land.
    std::promise<ClientResult> promise;
    auto future = promise.get_future();
    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
        return run_http3_client(&client_group.at(0), server_addr, cert_path, &promise);
    });
    ASSERT_EQ(future.wait_for(20s), std::future_status::ready);
    ClientResult result = future.get();
    EXPECT_FALSE(result.connected);
    EXPECT_NE(result.error, fiber::common::IoErr::None);

    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

TEST(Http3EndpointTest, RequiresTls) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    Server server(group.at(0), {});
    ASSERT_NE(server.add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
                      .address = {fiber::net::IpAddress::loopback_v4(), 0},
              }),
              nullptr);
    auto result = server.start();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), fiber::common::IoErr::Invalid);
    EXPECT_EQ(server.state(), Server::State::Created);

    group.stop();
    group.join();
}

TEST(Http3EndpointTest, OneShardPerWorkerLoop) {
    TestCredential tls;
    ASSERT_TRUE(tls.init());

    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup workers(3);
    group.start();
    workers.start();

    RunningServer running;
    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{}, &workers);
    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
            .tls = tls_options(*tls.credential),
            .handler = [](fiber::http::HttpExchange &exchange) { return write_text(exchange, 200, "ok"); },
    });
    ASSERT_NE(running.endpoint, nullptr);
    ASSERT_TRUE(running.server->start().has_value());
    spawn_serve(running, group.at(0));

    // Three SO_REUSEPORT shards behind one port.
    EXPECT_EQ(running.endpoint->shard_count(), 3u);
    EXPECT_NE(running.endpoint->local_addr().port(), 0);

    running.stop_and_join();
    EXPECT_EQ(running.server->state(), Server::State::Stopped);

    group.stop();
    group.join();
    workers.stop();
    workers.join();
}

// The Alt-Svc shape: one TCP endpoint on an ephemeral port, and an HTTP/3
// endpoint that follows it onto the same port number.
TEST(Http3EndpointTest, InheritsPortFromTcpEndpoint) {
    TestCredential tls;
    ASSERT_TRUE(tls.init());

    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    RunningServer running;
    running.server = std::make_unique<Server>(
            group.at(0), [](fiber::http::HttpExchange &exchange) { return write_text(exchange, 200, "shared-port"); });
    running.tcp_endpoint = running.server->add_endpoint<Http1Endpoint>(Http1Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
    });
    ASSERT_NE(running.tcp_endpoint, nullptr);
    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
            .inherit_port_from = running.tcp_endpoint,
            .tls = tls_options(*tls.credential),
    });
    ASSERT_NE(running.endpoint, nullptr);
    ASSERT_TRUE(running.server->start().has_value());
    spawn_serve(running, group.at(0));

    const std::uint16_t tcp_port = running.tcp_endpoint->local_addr().port();
    ASSERT_NE(tcp_port, 0);
    EXPECT_EQ(running.endpoint->local_addr().port(), tcp_port);

    // Both stacks answer on that port: HTTP/1 over TCP, HTTP/3 over UDP.
    int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(client, 0);
    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), tcp_port);
    sockaddr_storage storage{};
    socklen_t len = 0;
    ASSERT_TRUE(target.to_sockaddr(storage, len));
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&storage), len), 0);
    const char *request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));
    std::string tcp_response;
    char buffer[4096];
    for (;;) {
        ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        tcp_response.append(buffer, static_cast<std::size_t>(received));
    }
    ::close(client);
    EXPECT_NE(tcp_response.find("shared-port"), std::string::npos);

    std::promise<ClientResult> promise;
    auto future = promise.get_future();
    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();
    const std::string cert_path = tls.cert.path();
    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
        return run_http3_client(&client_group.at(0), server_addr, cert_path, &promise);
    });
    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
    ClientResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.status, 200);
    EXPECT_EQ(result.body, "shared-port");

    running.stop_and_join();
    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

// Pins the h3 side of the pipe contract: an Auto (no Content-Length) response
// streamed through pipe_http_body in unbuffered mode turns the source's empty
// terminator into a terminal-only (fin-only) write, whose success must consume
// the chain's completion marker so the pipe's completion-progress invariant
// holds. (The h2 sink violated this; see Http2EndpointTest's twin.)
constexpr std::string_view kStreamedBody = "0123456789abcdefghijklmnopqrstuvwxyz0123";

class FixedBodySource {
public:
    explicit FixedBodySource(std::string_view body) : body_(body) {}

    fiber::async::Task<fiber::common::IoResult<fiber::mem::IoBufChain>>
    read_body(std::size_t /*max_bytes*/, std::chrono::milliseconds /*timeout*/) noexcept {
        fiber::mem::IoBufChain chunk(fiber::event::EventLoop::current().io_buf_node_pool());
        if (!served_body_) {
            served_body_ = true;
            fiber::mem::IoBuf data = fiber::mem::IoBuf::allocate(body_.size());
            if (!data) {
                co_return std::unexpected(fiber::common::IoErr::NoMem);
            }
            std::memcpy(data.writable_data(), body_.data(), body_.size());
            data.commit(body_.size());
            if (!chunk.append(std::move(data))) {
                co_return std::unexpected(fiber::common::IoErr::NoMem);
            }
            co_return chunk; // payload, not complete yet
        }
        if (!served_terminator_) {
            served_terminator_ = true;
            chunk.mark_complete(); // empty terminator
            co_return chunk;
        }
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }

    fiber::common::IoResult<void> abort(fiber::common::IoErr /*reason*/) noexcept { return {}; }

private:
    std::string_view body_;
    bool served_body_ = false;
    bool served_terminator_ = false;
};

TEST(Http3EndpointTest, StreamedAutoBodyThroughPipeCompletes) {
    TestCredential tls;
    ASSERT_TRUE(tls.init());

    fiber::event::EventLoopGroup group(1);
    fiber::event::EventLoopGroup client_group(1);
    group.start();
    client_group.start();

    std::promise<fiber::http::HttpBodyPipeResult> pipe_promise;
    auto pipe_future = pipe_promise.get_future();

    RunningServer running;
    running.server = std::make_unique<Server>(group.at(0), fiber::http::HttpHandler{});
    running.endpoint = running.server->add_endpoint<Http3Endpoint>(Http3Endpoint::Options{
            .address = {fiber::net::IpAddress::loopback_v4(), 0},
            .tls = tls_options(*tls.credential),
            .handler = [&pipe_promise](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
                fiber::http::HttpHeaders headers(exchange.pool());
                auto sent = co_await exchange.send_header(
                        {
                                .kind = fiber::http::OutgoingHeaderKind::Final,
                                .status_code = 200,
                                .headers = &headers,
                                .body = fiber::http::HttpBodySpec::Auto(),
                                .end_stream = false,
                        },
                        5s);
                if (!sent) {
                    pipe_promise.set_value(std::unexpected(fiber::http::HttpBodyPipeError{
                            .code = sent.error(), .phase = fiber::http::HttpBodyPipePhase::Validate}));
                    co_return;
                }
                FixedBodySource source(kStreamedBody);
                auto writer = fiber::http::make_http_response_writer(exchange);
                auto piped =
                        co_await fiber::http::pipe_http_body(fiber::http::make_http_body_pipe_reader(source),
                                                             fiber::http::make_http_body_pipe_writer(writer),
                                                             fiber::event::EventLoop::current().io_buf_node_pool(),
                                                             {.low_water = fiber::http::kUnbufferedBodyPipeLowWater,
                                                              .read_timeout = 5s,
                                                              .write_timeout = 5s});
                pipe_promise.set_value(std::move(piped));
                co_return;
            },
    });
    ASSERT_NE(running.endpoint, nullptr);
    ASSERT_TRUE(running.server->start().has_value());
    spawn_serve(running, group.at(0));

    const fiber::net::SocketAddress server_addr = running.endpoint->local_addr();

    std::promise<ClientResult> promise;
    auto future = promise.get_future();
    const std::string cert_path = tls.cert.path();
    fiber::async::spawn(client_group.at(0), [&client_group, server_addr, cert_path, &promise]() {
        return run_http3_client(&client_group.at(0), server_addr, cert_path, &promise);
    });

    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
    ClientResult result = future.get();
    EXPECT_EQ(result.error, fiber::common::IoErr::None);
    EXPECT_EQ(result.status, 200);
    EXPECT_EQ(result.body, kStreamedBody);

    ASSERT_EQ(pipe_future.wait_for(15s), std::future_status::ready);
    auto piped = pipe_future.get();
    ASSERT_TRUE(piped.has_value());
    EXPECT_EQ(piped->bytes_written, kStreamedBody.size());

    running.stop_and_join();
    EXPECT_EQ(running.server->state(), Server::State::Stopped);

    group.stop();
    group.join();
    client_group.stop();
    client_group.join();
}

} // namespace
