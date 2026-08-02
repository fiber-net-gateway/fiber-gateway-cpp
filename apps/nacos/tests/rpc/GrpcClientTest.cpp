#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include <google/protobuf/message_lite.h>

#include "async/Spawn.h"
#include "async/Task.h"
#include "common/IoError.h"
#include "common/mem/IoBuf.h"
#include "common/mem/IoBufChain.h"
#include "event/EventLoopGroup.h"
#include "helloworld.pb.h"
#include "http/HttpBodySpec.h"
#include "http/HttpCommon.h"
#include "http/HttpExchange.h"
#include "http/HttpExchangeIo.h"
#include "http/HttpHeaders.h"
#include "http/HttpServer.h"
#include "net/IpAddress.h"
#include "net/SocketAddress.h"
#include "rpc/grpc/GrpcClient.h"

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;

// Generates a fresh localhost self-signed cert/key via openssl at test time.
struct TlsCert {
    std::string cert_path;
    std::string key_path;
    bool ok = false;

    TlsCert() {
        const auto pid = ::getpid();
        cert_path = "/tmp/fiber_grpc_test_cert_" + std::to_string(pid) + ".pem";
        key_path = "/tmp/fiber_grpc_test_key_" + std::to_string(pid) + ".pem";
        const std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -keyout " + key_path + " -out " + cert_path +
                                " -days 1 -subj /CN=localhost -addext subjectAltName=DNS:localhost "
                                ">/dev/null 2>&1";
        ok = (::system(cmd.c_str()) == 0);
    }
    ~TlsCert() {
        if (ok) {
            std::remove(cert_path.c_str());
            std::remove(key_path.c_str());
        }
    }
    TlsCert(const TlsCert &) = delete;
    TlsCert &operator=(const TlsCert &) = delete;
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

fiber::async::Task<fiber::common::IoResult<std::string>> read_body_to_string(fiber::http::HttpExchange &exchange) {
    std::string out;
    for (;;) {
        auto chunk_result = co_await exchange.read_body(64 * 1024);
        if (!chunk_result) {
            co_return std::unexpected(chunk_result.error());
        }
        const bool last = chunk_result->complete();
        out.append(chain_to_string(std::move(*chunk_result)));
        if (last) {
            break;
        }
    }
    co_return out;
}

// Build a gRPC length-prefixed frame (5-byte header + payload) as a string.
std::string grpc_frame(std::string_view payload) {
    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    std::string out;
    out.push_back('\x00');
    out.push_back(static_cast<char>((len >> 24) & 0xff));
    out.push_back(static_cast<char>((len >> 16) & 0xff));
    out.push_back(static_cast<char>((len >> 8) & 0xff));
    out.push_back(static_cast<char>(len & 0xff));
    out.append(payload.data(), payload.size());
    return out;
}

fiber::async::Task<void> say_hello_handler(fiber::http::HttpExchange &exchange) {
    auto body_result = co_await read_body_to_string(exchange);
    if (!body_result) {
        co_return;
    }
    const std::string &body = *body_result;
    if (body.size() < 5) {
        co_return;
    }

    helloworld::HelloRequest request;
    if (!request.ParseFromArray(body.data() + 5, static_cast<int>(body.size() - 5))) {
        co_return;
    }

    helloworld::HelloReply reply;
    reply.set_message("Hello, " + request.name());
    reply.set_count(request.items_size());

    std::string reply_bytes;
    if (!reply.SerializeToString(&reply_bytes)) {
        co_return;
    }
    const std::string framed = grpc_frame(reply_bytes);

    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("content-type", "application/grpc");
    auto header_result = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::None(),
            .end_stream = false,
    });
    if (!header_result) {
        co_return;
    }

    auto write_result =
            co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(framed.data()), framed.size(), false);
    if (!write_result) {
        co_return;
    }

    fiber::http::HttpHeaders trailers(exchange.pool());
    trailers.set("grpc-status", "0");
    (void) co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Trailer,
            .headers = &trailers,
            .body = fiber::http::HttpBodySpec::None(),
            .end_stream = true,
    });
}

fiber::async::Task<void> fail_handler(fiber::http::HttpExchange &exchange) {
    // Drain the request body.
    for (;;) {
        auto chunk = co_await exchange.read_body(64 * 1024);
        if (!chunk || chunk->complete()) {
            break;
        }
    }

    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("content-type", "application/grpc");
    (void) co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::None(),
            .end_stream = false,
    });

    fiber::http::HttpHeaders trailers(exchange.pool());
    trailers.set("grpc-status", "5");
    trailers.set("grpc-message", "intentional failure");
    (void) co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Trailer,
            .headers = &trailers,
            .body = fiber::http::HttpBodySpec::None(),
            .end_stream = true,
    });
}

fiber::http::HttpHandler make_grpc_handler() {
    return [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        const std::string_view path = exchange.uri().path;
        if (path == "/helloworld.Greeter/SayHello") {
            co_await say_hello_handler(exchange);
        } else if (path == "/helloworld.Greeter/Fail") {
            co_await fail_handler(exchange);
        }
        co_return;
    };
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

DetachedTask close_server_on_loop(fiber::http::HttpServer *server, std::promise<void> *done_promise) {
    if (server) {
        server->close();
    }
    done_promise->set_value();
    co_return;
}

struct ClientResult {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    fiber::nacos::detail::grpc::GrpcStatus status{};
    helloworld::HelloReply reply{};
    bool got_result = false;
};

struct TwoCallResult {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    fiber::nacos::detail::grpc::GrpcStatus status1{};
    fiber::nacos::detail::grpc::GrpcStatus status2{};
    helloworld::HelloReply reply1{};
    helloworld::HelloReply reply2{};
    bool got1 = false;
    bool got2 = false;
};

DetachedTask wait_for_client_connection(fiber::nacos::detail::grpc::GrpcClient *client, bool *returned = nullptr) {
    (void) co_await client->wait_closed();
    if (returned) {
        *returned = true;
    }
}

fiber::async::Task<fiber::common::IoResult<fiber::nacos::detail::grpc::GrpcStatus>>
call_unary(fiber::nacos::detail::grpc::GrpcClient &client, std::string_view service, std::string_view method,
           const google::protobuf::MessageLite &request, google::protobuf::MessageLite &response,
           fiber::mem::BufPool &pool) {
    fiber::nacos::detail::grpc::GrpcStream stream = client.open_stream(service, method, pool);
    if (auto result = co_await stream.open(); !result) {
        co_return std::unexpected(result.error());
    }
    if (auto result = co_await stream.write(request); !result) {
        co_return std::unexpected(result.error());
    }
    if (auto result = co_await stream.writes_done(); !result) {
        co_return std::unexpected(result.error());
    }

    const auto first = co_await stream.read(response);
    if (!first) {
        co_return std::unexpected(first.error());
    }
    if (*first == fiber::nacos::detail::grpc::GrpcReadOutcome::Message) {
        const auto second = co_await stream.read(response);
        if (!second) {
            co_return std::unexpected(second.error());
        }
        if (*second == fiber::nacos::detail::grpc::GrpcReadOutcome::Message) {
            co_return std::unexpected(fiber::common::IoErr::Invalid);
        }
    }

    auto finish_result = co_await stream.finish();
    if (!finish_result) {
        co_return std::unexpected(finish_result.error());
    }
    if (!finish_result->ok()) {
        co_return *finish_result;
    }
    if (*first != fiber::nacos::detail::grpc::GrpcReadOutcome::Message) {
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }
    co_return *finish_result;
}

DetachedTask run_client(fiber::event::EventLoop *loop, std::uint16_t port, std::string_view service,
                        std::string_view method, const helloworld::HelloRequest &request,
                        std::shared_ptr<std::promise<ClientResult>> promise) {
    ClientResult result;
    fiber::nacos::detail::grpc::GrpcClient::Options options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.tls.enabled = true;
    options.tls.server_name = "localhost";
    options.authority = "localhost";
    options.scheme = "https";

    fiber::nacos::detail::grpc::GrpcClient client(*loop, options);
    auto connect_result = co_await client.connect(5s);
    if (!connect_result) {
        result.err = connect_result.error();
        promise->set_value(std::move(result));
        co_return;
    }
    fiber::async::spawn(*loop, [&client]() { return wait_for_client_connection(&client); });

    fiber::mem::BufPool pool;
    helloworld::HelloRequest req = request;
    helloworld::HelloReply reply;
    auto call_result = co_await call_unary(client, service, method, req, reply, pool);
    if (!call_result) {
        result.err = call_result.error();
    } else {
        result.status = std::move(*call_result);
        result.reply = std::move(reply);
        result.got_result = true;
    }

    auto close_result = co_await client.graceful_shutdown();
    if (!close_result && result.err == fiber::common::IoErr::None) {
        result.err = close_result.error();
    }

    promise->set_value(std::move(result));
}

DetachedTask run_client_two_calls(fiber::event::EventLoop *loop, std::uint16_t port,
                                  const helloworld::HelloRequest &request,
                                  std::shared_ptr<std::promise<TwoCallResult>> promise) {
    TwoCallResult result;
    fiber::nacos::detail::grpc::GrpcClient::Options options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.tls.enabled = true;
    options.tls.server_name = "localhost";
    // The second call exercises repeated request encoding on the same HTTP/2
    // connection without relying on HPACK dynamic-table state.
    options.authority = "localhost";
    options.scheme = "https";

    fiber::nacos::detail::grpc::GrpcClient client(*loop, options);
    auto connect_result = co_await client.connect(5s);
    if (!connect_result) {
        result.err = connect_result.error();
        promise->set_value(std::move(result));
        co_return;
    }
    fiber::async::spawn(*loop, [&client]() { return wait_for_client_connection(&client); });

    fiber::mem::BufPool pool;
    helloworld::HelloRequest req = request;
    for (int call = 0; call < 2; ++call) {
        helloworld::HelloReply reply;
        auto call_result = co_await call_unary(client, "helloworld.Greeter", "SayHello", req, reply, pool);
        if (!call_result) {
            result.err = call_result.error();
            break;
        }
        if (call == 0) {
            result.status1 = std::move(*call_result);
            result.reply1 = std::move(reply);
            result.got1 = true;
        } else {
            result.status2 = std::move(*call_result);
            result.reply2 = std::move(reply);
            result.got2 = true;
        }
    }

    co_await client.shutdown();

    promise->set_value(std::move(result));
}

struct LifecycleResult {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    bool moved_from_invalid = false;
    bool wait_returned = false;
    bool repeated_shutdown_completed = false;
    bool repeated_wait_succeeded = false;
};

DetachedTask run_client_lifecycle(fiber::event::EventLoop *loop, std::uint16_t port,
                                  std::shared_ptr<std::promise<LifecycleResult>> promise) {
    LifecycleResult result;
    fiber::nacos::detail::grpc::GrpcClient::Options options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.tls.enabled = true;
    options.tls.server_name = "localhost";
    options.authority = "localhost";
    options.scheme = "https";

    fiber::nacos::detail::grpc::GrpcClient client(*loop, options);
    auto connect_result = co_await client.connect(5s);
    if (!connect_result) {
        result.err = connect_result.error();
        promise->set_value(std::move(result));
        co_return;
    }

    fiber::async::spawn(*loop,
                        [&client, &result]() { return wait_for_client_connection(&client, &result.wait_returned); });
    {
        fiber::mem::BufPool pool;
        fiber::nacos::detail::grpc::GrpcStream original = client.open_stream("helloworld.Greeter", "SayHello", pool);
        fiber::nacos::detail::grpc::GrpcStream moved = std::move(original);
        result.moved_from_invalid = !original.valid() && moved.valid();
    }

    // shutdown() can start before the posted observer enters. Both it and the
    // observer must be released by the same connection-close completion.
    co_await client.shutdown();
    co_await client.shutdown();
    result.repeated_shutdown_completed = true;

    auto second_wait = co_await client.wait_closed();
    result.repeated_wait_succeeded = second_wait.has_value();
    promise->set_value(std::move(result));
}

TEST(GrpcClientTest, StreamUnaryRoundTrip) {
    TlsCert cert;
    ASSERT_TRUE(cert.ok) << "openssl cert generation failed";

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::http::HttpServerOptions server_options;
    server_options.tls.enabled = true;
    server_options.tls.cert_file = cert.cert_path;
    server_options.tls.key_file = cert.key_path;
    server_options.tls.alpn = {"h2"};

    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return start_http_server(&group.at(0), make_grpc_handler(), std::move(server_options), &port_promise,
                                 &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    helloworld::HelloRequest request;
    request.set_name("fiber");
    request.add_items("a");
    request.add_items("bb");

    auto promise = std::make_shared<std::promise<ClientResult>>();
    auto future = promise->get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_client(&group.at(0), port, "helloworld.Greeter", "SayHello", request, promise);
    });

    ClientResult result = future.get();
    ASSERT_EQ(result.err, fiber::common::IoErr::None) << "transport error";
    ASSERT_TRUE(result.got_result);
    EXPECT_TRUE(result.status.ok()) << "grpc code=" << result.status.code;
    EXPECT_EQ(result.status.code, 0);
    EXPECT_EQ(result.reply.message(), "Hello, fiber");
    EXPECT_EQ(result.reply.count(), 2);

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_server_on_loop(server, &close_promise); });
    close_future.get();
    group.stop();
    group.join();
    delete server;
}

TEST(GrpcClientTest, StreamUnaryReturnsGrpcError) {
    TlsCert cert;
    ASSERT_TRUE(cert.ok) << "openssl cert generation failed";

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::http::HttpServerOptions server_options;
    server_options.tls.enabled = true;
    server_options.tls.cert_file = cert.cert_path;
    server_options.tls.key_file = cert.key_path;
    server_options.tls.alpn = {"h2"};

    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return start_http_server(&group.at(0), make_grpc_handler(), std::move(server_options), &port_promise,
                                 &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    helloworld::HelloRequest request;
    request.set_name("boom");

    auto promise = std::make_shared<std::promise<ClientResult>>();
    auto future = promise->get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_client(&group.at(0), port, "helloworld.Greeter", "Fail", request, promise);
    });

    ClientResult result = future.get();
    ASSERT_EQ(result.err, fiber::common::IoErr::None) << "transport error";
    ASSERT_TRUE(result.got_result);
    EXPECT_FALSE(result.status.ok());
    EXPECT_EQ(result.status.code, 5);
    EXPECT_EQ(result.status.message, "intentional failure");

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_server_on_loop(server, &close_promise); });
    close_future.get();
    group.stop();
    group.join();
    delete server;
}

TEST(GrpcClientTest, RepeatedUnaryCallsWorkWithoutHpackDynamicTable) {
    TlsCert cert;
    ASSERT_TRUE(cert.ok) << "openssl cert generation failed";

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::http::HttpServerOptions server_options;
    server_options.tls.enabled = true;
    server_options.tls.cert_file = cert.cert_path;
    server_options.tls.key_file = cert.key_path;
    server_options.tls.alpn = {"h2"};

    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return start_http_server(&group.at(0), make_grpc_handler(), std::move(server_options), &port_promise,
                                 &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    helloworld::HelloRequest request;
    request.set_name("fiber");
    request.add_items("a");
    request.add_items("bb");

    auto promise = std::make_shared<std::promise<TwoCallResult>>();
    auto future = promise->get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_client_two_calls(&group.at(0), port, request, promise); });

    TwoCallResult result = future.get();
    ASSERT_EQ(result.err, fiber::common::IoErr::None) << "transport error";
    ASSERT_TRUE(result.got1);
    ASSERT_TRUE(result.got2);
    EXPECT_TRUE(result.status1.ok()) << "grpc code=" << result.status1.code;
    EXPECT_EQ(result.status1.code, 0);
    EXPECT_EQ(result.reply1.message(), "Hello, fiber");
    EXPECT_EQ(result.reply1.count(), 2);
    EXPECT_TRUE(result.status2.ok()) << "grpc code=" << result.status2.code;
    EXPECT_EQ(result.status2.code, 0);
    EXPECT_EQ(result.reply2.message(), "Hello, fiber");
    EXPECT_EQ(result.reply2.count(), 2);

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_server_on_loop(server, &close_promise); });
    close_future.get();
    group.stop();
    group.join();
    delete server;
}

TEST(GrpcClientTest, ImmediateShutdownReleasesAllConnectionCloseWaiters) {
    TlsCert cert;
    ASSERT_TRUE(cert.ok) << "openssl cert generation failed";

    fiber::event::EventLoopGroup group(1);
    group.start();

    fiber::http::HttpServerOptions server_options;
    server_options.tls.enabled = true;
    server_options.tls.cert_file = cert.cert_path;
    server_options.tls.key_file = cert.key_path;
    server_options.tls.alpn = {"h2"};

    std::promise<std::uint16_t> port_promise;
    std::promise<fiber::http::HttpServer *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return start_http_server(&group.at(0), make_grpc_handler(), std::move(server_options), &port_promise,
                                 &server_promise);
    });

    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);
    const std::uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    auto promise = std::make_shared<std::promise<LifecycleResult>>();
    auto future = promise->get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_client_lifecycle(&group.at(0), port, promise); });

    LifecycleResult result = future.get();
    ASSERT_EQ(result.err, fiber::common::IoErr::None);
    EXPECT_TRUE(result.moved_from_invalid);
    EXPECT_TRUE(result.wait_returned);
    EXPECT_TRUE(result.repeated_shutdown_completed);
    EXPECT_TRUE(result.repeated_wait_succeeded);

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_server_on_loop(server, &close_promise); });
    close_future.get();
    group.stop();
    group.join();
    delete server;
}

} // namespace
