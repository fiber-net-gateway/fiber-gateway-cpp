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
};

// One HTTP/3 request over a fresh QUIC connection. `hold` (when valid) keeps
// the connection open after the response so a test can drain the server while
// a client is still attached.
DetachedTask run_http3_client(fiber::event::EventLoop *loop, fiber::net::SocketAddress server_addr,
                              std::string cert_path, std::promise<ClientResult> *promise,
                              std::shared_ptr<std::promise<void>> done_first = {}, std::shared_future<void> hold = {}) {
    ClientResult result{};
    fiber::quic::QuicUdpEndpoint endpoint;
    fiber::quic::QuicUdpEndpoint::EndpointOptions endpoint_options{};
    endpoint_options.bind_addr = {fiber::net::IpAddress::loopback_v4(), 0};
    auto endpoint_ready = endpoint.init(*loop, endpoint_options);
    if (!endpoint_ready) {
        result.error = endpoint_ready.error();
        promise->set_value(std::move(result));
        co_return;
    }

    auto trust_store = fiber::net::TrustStore::create(fiber::net::TrustStoreOptions::from_file(cert_path));
    if (!trust_store) {
        result.error = trust_store.error();
        endpoint.close();
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
        endpoint.close();
        promise->set_value(std::move(result));
        co_return;
    }
    auto initialized = client.init();
    if (!initialized) {
        result.error = initialized.error();
        endpoint.close();
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
        endpoint.close();
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
    endpoint.close();
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
                                                             {.read_timeout = 5s,
                                                              .low_water = fiber::http::kUnbufferedBodyPipeLowWater,
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
