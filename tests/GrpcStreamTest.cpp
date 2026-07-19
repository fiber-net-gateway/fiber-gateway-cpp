#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <google/protobuf/message_lite.h>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "async/Task.h"
#include "common/IoError.h"
#include "event/EventLoop.h"
#include "event/EventLoopGroup.h"
#include "grpc/GrpcClient.h"
#include "grpc/GrpcStream.h"
#include "helloworld.pb.h"
#include "http/HttpBodySpec.h"
#include "http/HttpCommon.h"
#include "http/HttpExchange.h"
#include "http/HttpExchangeIo.h"
#include "http/HttpHeaders.h"
#include "http/HttpServer.h"
#include "net/IpAddress.h"
#include "net/SocketAddress.h"

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;
using fiber::async::Task;

struct TlsCert {
    std::string cert_path;
    std::string key_path;
    bool ok = false;

    TlsCert() {
        const auto pid = ::getpid();
        cert_path = "/tmp/fiber_grpc_stream_test_cert_" + std::to_string(pid) + ".pem";
        key_path = "/tmp/fiber_grpc_stream_test_key_" + std::to_string(pid) + ".pem";
        const std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -keyout " + key_path + " -out " + cert_path +
                                " -days 1 -subj /CN=localhost -addext subjectAltName=DNS:localhost >/dev/null 2>&1";
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

Task<fiber::common::IoResult<std::string>> read_body_to_string(fiber::http::HttpExchange &exchange) {
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

std::size_t count_frames(const std::string &body) {
    std::size_t pos = 0;
    std::size_t count = 0;
    while (pos + 5 <= body.size()) {
        const std::uint32_t len =
                (static_cast<std::uint8_t>(body[pos + 1]) << 24) | (static_cast<std::uint8_t>(body[pos + 2]) << 16) |
                (static_cast<std::uint8_t>(body[pos + 3]) << 8) | static_cast<std::uint8_t>(body[pos + 4]);
        if (pos + 5 + len > body.size()) {
            break;
        }
        ++count;
        pos += 5 + len;
    }
    return count;
}

// ---- server handlers ----

Task<void> say_hello_stream_handler(fiber::http::HttpExchange &exchange) {
    auto body_result = co_await read_body_to_string(exchange);
    if (!body_result) {
        co_return;
    }
    helloworld::HelloRequest request;
    if (body_result->size() >= 5) {
        (void) request.ParseFromArray(body_result->data() + 5, static_cast<int>(body_result->size() - 5));
    }

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

    for (int i = 0; i < 3; ++i) {
        helloworld::HelloReply reply;
        reply.set_message("stream " + std::to_string(i));
        reply.set_count(i);
        std::string bytes;
        reply.SerializeToString(&bytes);
        const std::string framed = grpc_frame(bytes);
        auto write_result = co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(framed.data()),
                                                         framed.size(), false);
        if (!write_result) {
            co_return;
        }
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

Task<void> sum_stream_handler(fiber::http::HttpExchange &exchange) {
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

    auto body_result = co_await read_body_to_string(exchange);
    if (!body_result) {
        co_return;
    }
    const std::size_t n = count_frames(*body_result);

    helloworld::HelloReply reply;
    reply.set_count(static_cast<std::int64_t>(n));
    reply.set_message("sum");
    std::string bytes;
    reply.SerializeToString(&bytes);
    const std::string framed = grpc_frame(bytes);
    (void) co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(framed.data()), framed.size(), false);

    fiber::http::HttpHeaders trailers(exchange.pool());
    trailers.set("grpc-status", "0");
    (void) co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Trailer,
            .headers = &trailers,
            .body = fiber::http::HttpBodySpec::None(),
            .end_stream = true,
    });
}

// Bidi: echo each received request frame back as a reply frame.
Task<void> chat_handler(fiber::http::HttpExchange &exchange) {
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

    auto body_result = co_await read_body_to_string(exchange);
    if (!body_result) {
        co_return;
    }
    const std::string &body = *body_result;
    std::size_t pos = 0;
    while (pos + 5 <= body.size()) {
        const std::uint32_t len =
                (static_cast<std::uint8_t>(body[pos + 1]) << 24) | (static_cast<std::uint8_t>(body[pos + 2]) << 16) |
                (static_cast<std::uint8_t>(body[pos + 3]) << 8) | static_cast<std::uint8_t>(body[pos + 4]);
        if (pos + 5 + len > body.size()) {
            break;
        }
        const std::string payload = body.substr(pos + 5, len);
        const std::string framed = grpc_frame(payload);
        auto write_result = co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(framed.data()),
                                                         framed.size(), false);
        if (!write_result) {
            co_return;
        }
        pos += 5 + len;
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

// Trailers-only error: response HEADERS carries grpc-status + END_STREAM.
Task<void> stream_fail_handler(fiber::http::HttpExchange &exchange) {
    for (;;) {
        auto chunk = co_await exchange.read_body(64 * 1024);
        if (!chunk || chunk->complete()) {
            break;
        }
    }
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("content-type", "application/grpc");
    headers.set("grpc-status", "5");
    headers.set("grpc-message", "stream failure");
    (void) co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::None(),
            .end_stream = true,
    });
}

// Stall: send 200 headers, then block reading request body (the client never
// half-closes). Used to exercise client-side cancellation of a blocked read.
Task<void> stall_handler(fiber::http::HttpExchange &exchange) {
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("content-type", "application/grpc");
    (void) co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::None(),
            .end_stream = false,
    });
    for (;;) {
        auto chunk = co_await exchange.read_body(64 * 1024);
        if (!chunk || chunk->complete()) {
            break; // read error (client RST) or unexpected END_STREAM
        }
    }
}

// Trailers-only error carrying an oversized grpc-message, to verify the client
// caps the untrusted value instead of copying it whole into a std::string.
Task<void> oversize_fail_handler(fiber::http::HttpExchange &exchange) {
    for (;;) {
        auto chunk = co_await exchange.read_body(64 * 1024);
        if (!chunk || chunk->complete()) {
            break;
        }
    }
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("content-type", "application/grpc");
    headers.set("grpc-status", "5");
    headers.set("grpc-message", std::string(16 * 1024, 'x'));
    (void) co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::None(),
            .end_stream = true,
    });
}

fiber::http::HttpHandler make_grpc_handler() {
    return [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        const std::string_view path = exchange.uri().path;
        if (path == "/helloworld.Greeter/SayHelloStream") {
            co_await say_hello_stream_handler(exchange);
        } else if (path == "/helloworld.Greeter/SumStream") {
            co_await sum_stream_handler(exchange);
        } else if (path == "/helloworld.Greeter/Chat") {
            co_await chat_handler(exchange);
        } else if (path == "/helloworld.Greeter/StreamFail") {
            co_await stream_fail_handler(exchange);
        } else if (path == "/helloworld.Greeter/OversizeFail") {
            co_await oversize_fail_handler(exchange);
        } else if (path == "/helloworld.Greeter/Stall") {
            co_await stall_handler(exchange);
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
    // Yield so the close is processed on the loop before we resolve.
    co_await fiber::async::sleep(10ms);
    done_promise->set_value();
    co_return;
}

enum class Scenario {
    ServerStream,
    ClientStream,
    Bidi,
    TrailersOnlyError,
    Cancel,
    Deadline,
    OversizeMessage,
};

struct Result {
    fiber::common::IoErr err = fiber::common::IoErr::None;
    bool finish_ok = false;
    fiber::grpc::GrpcStatus status{};
    std::vector<std::string> messages;
    std::vector<long long> counts;
};

Task<Result> drive(fiber::grpc::GrpcStream &s, Scenario sc) {
    Result r;

    switch (sc) {
        case Scenario::ServerStream: {
            auto open_result = co_await s.open();
            if (!open_result) {
                r.err = open_result.error();
                break;
            }
            helloworld::HelloRequest req;
            req.set_name("fiber");
            if (auto w = co_await s.write(req); !w) {
                r.err = w.error();
                break;
            }
            if (auto wd = co_await s.writes_done(); !wd) {
                r.err = wd.error();
                break;
            }
            for (;;) {
                helloworld::HelloReply reply;
                auto rr = co_await s.read(reply);
                if (!rr) {
                    r.err = rr.error();
                    break;
                }
                if (*rr == fiber::grpc::GrpcReadOutcome::End) {
                    break;
                }
                r.messages.push_back(reply.message());
                r.counts.push_back(reply.count());
            }
            if (r.err == fiber::common::IoErr::None) {
                if (auto f = co_await s.finish(); f) {
                    r.finish_ok = true;
                    r.status = *f;
                } else {
                    r.err = f.error();
                }
            }
            break;
        }
        case Scenario::ClientStream: {
            auto open_result = co_await s.open();
            if (!open_result) {
                r.err = open_result.error();
                break;
            }
            for (int i = 0; i < 3; ++i) {
                helloworld::HelloRequest req;
                req.set_name("req" + std::to_string(i));
                req.set_num(i);
                if (auto w = co_await s.write(req); !w) {
                    r.err = w.error();
                    break;
                }
            }
            if (r.err == fiber::common::IoErr::None) {
                if (auto wd = co_await s.writes_done(); !wd) {
                    r.err = wd.error();
                }
            }
            if (r.err == fiber::common::IoErr::None) {
                helloworld::HelloReply reply;
                if (auto rr = co_await s.read(reply); rr && *rr == fiber::grpc::GrpcReadOutcome::Message) {
                    r.messages.push_back(reply.message());
                    r.counts.push_back(reply.count());
                } else if (!rr) {
                    r.err = rr.error();
                }
            }
            if (r.err == fiber::common::IoErr::None) {
                if (auto f = co_await s.finish(); f) {
                    r.finish_ok = true;
                    r.status = *f;
                } else {
                    r.err = f.error();
                }
            }
            break;
        }
        case Scenario::Bidi: {
            auto open_result = co_await s.open();
            if (!open_result) {
                r.err = open_result.error();
                break;
            }
            for (int i = 0; i < 3; ++i) {
                helloworld::HelloRequest req;
                req.set_name("c" + std::to_string(i));
                req.set_num(i);
                if (auto w = co_await s.write(req); !w) {
                    r.err = w.error();
                    break;
                }
            }
            if (r.err == fiber::common::IoErr::None) {
                if (auto wd = co_await s.writes_done(); !wd) {
                    r.err = wd.error();
                }
            }
            while (r.err == fiber::common::IoErr::None) {
                helloworld::HelloRequest echo;
                auto rr = co_await s.read(echo);
                if (!rr) {
                    r.err = rr.error();
                    break;
                }
                if (*rr == fiber::grpc::GrpcReadOutcome::End) {
                    break;
                }
                r.messages.push_back(echo.name());
                r.counts.push_back(echo.num());
            }
            if (r.err == fiber::common::IoErr::None) {
                if (auto f = co_await s.finish(); f) {
                    r.finish_ok = true;
                    r.status = *f;
                } else {
                    r.err = f.error();
                }
            }
            break;
        }
        case Scenario::TrailersOnlyError: {
            auto open_result = co_await s.open();
            if (!open_result) {
                r.err = open_result.error();
                break;
            }
            helloworld::HelloRequest req;
            req.set_name("boom");
            if (auto w = co_await s.write(req); !w) {
                r.err = w.error();
                break;
            }
            if (auto wd = co_await s.writes_done(); !wd) {
                r.err = wd.error();
                break;
            }
            helloworld::HelloReply reply;
            if (auto rr = co_await s.read(reply); !rr) {
                r.err = rr.error();
            }
            if (r.err == fiber::common::IoErr::None) {
                if (auto f = co_await s.finish(); f) {
                    r.finish_ok = true;
                    r.status = *f;
                } else {
                    r.err = f.error();
                }
            }
            break;
        }
        case Scenario::Cancel: {
            auto open_result = co_await s.open();
            if (!open_result) {
                r.err = open_result.error();
                break;
            }
            helloworld::HelloRequest req;
            req.set_name("x");
            if (auto w = co_await s.write(req); !w) {
                r.err = w.error();
                break;
            }
            // The client never half-closes; cancel after a short delay while the
            // blocked read waits for a message the server will never send.
            fiber::async::spawn(fiber::event::EventLoop::current(), [&s]() -> DetachedTask {
                co_await fiber::async::sleep(10ms);
                s.cancel();
                co_return;
            });
            helloworld::HelloReply reply;
            if (auto rr = co_await s.read(reply); !rr) {
                r.err = rr.error();
            }
            if (auto f = co_await s.finish(); !f) {
                if (r.err == fiber::common::IoErr::None) {
                    r.err = f.error();
                }
            } else {
                r.finish_ok = true;
                r.status = *f;
            }
            break;
        }
        case Scenario::Deadline: {
            // The server stalls after sending 200 headers (it blocks reading the
            // request body, which the client never half-closes). The client's
            // read() must hit the call deadline and fail with TimedOut instead
            // of blocking indefinitely.
            auto open_result = co_await s.open();
            if (!open_result) {
                r.err = open_result.error();
                break;
            }
            helloworld::HelloRequest req;
            req.set_name("x");
            if (auto w = co_await s.write(req); !w) {
                r.err = w.error();
                break;
            }
            helloworld::HelloReply reply;
            if (auto rr = co_await s.read(reply); !rr) {
                r.err = rr.error();
                break;
            }
            // Not expected: the read should time out before any message arrives.
            r.finish_ok = false;
            break;
        }
        case Scenario::OversizeMessage: {
            // Server returns a trailers-only error whose grpc-message is far
            // larger than the client's cap. The call must still succeed and the
            // message must be truncated, not copied whole.
            auto open_result = co_await s.open();
            if (!open_result) {
                r.err = open_result.error();
                break;
            }
            helloworld::HelloRequest req;
            req.set_name("big");
            if (auto w = co_await s.write(req); !w) {
                r.err = w.error();
                break;
            }
            if (auto wd = co_await s.writes_done(); !wd) {
                r.err = wd.error();
                break;
            }
            helloworld::HelloReply reply;
            if (auto rr = co_await s.read(reply); !rr) {
                r.err = rr.error();
                break;
            }
            if (auto f = co_await s.finish(); f) {
                r.finish_ok = true;
                r.status = std::move(*f);
            } else {
                r.err = f.error();
            }
            break;
        }
    }

    co_return r;
}

DetachedTask drive_client_connection(fiber::grpc::GrpcClient *client) { (void) co_await client->run(); }

DetachedTask run_client(fiber::event::EventLoop *loop, std::uint16_t port, Scenario sc,
                        std::shared_ptr<std::promise<Result>> promise) {
    Result result;
    fiber::grpc::GrpcClient::Options options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.tls.enabled = true;
    options.tls.server_name = "localhost";
    options.authority = "localhost";
    options.scheme = "https";

    fiber::grpc::GrpcClient client(*loop, options);
    auto connect_result = co_await client.connect(5s);
    if (!connect_result) {
        result.err = connect_result.error();
    } else {
        fiber::async::spawn(*loop, [&client]() { return drive_client_connection(&client); });
        fiber::mem::BufPool pool;
        const char *service = "helloworld.Greeter";
        const char *method = nullptr;
        switch (sc) {
            case Scenario::ServerStream:
                method = "SayHelloStream";
                break;
            case Scenario::ClientStream:
                method = "SumStream";
                break;
            case Scenario::Bidi:
                method = "Chat";
                break;
            case Scenario::TrailersOnlyError:
                method = "StreamFail";
                break;
            case Scenario::Cancel:
                method = "Stall";
                break;
            case Scenario::Deadline:
                method = "Stall";
                break;
            case Scenario::OversizeMessage:
                method = "OversizeFail";
                break;
        }
        fiber::grpc::GrpcStream::Options stream_opts;
        if (sc == Scenario::Deadline) {
            stream_opts.deadline = 100ms;
        }
        fiber::grpc::GrpcStream stream = client.open_stream(service, method, pool, stream_opts);
        result = co_await drive(stream, sc);
    }

    co_await client.shutdown();
    promise->set_value(std::move(result));
    co_return;
}

struct ServerCtx {
    TlsCert cert;
    std::uint16_t port = 0;
    fiber::http::HttpServer *server = nullptr;

    bool start(fiber::event::EventLoop &loop) {
        if (!cert.ok) {
            return false;
        }
        fiber::http::HttpServerOptions server_options;
        server_options.tls.enabled = true;
        server_options.tls.cert_file = cert.cert_path;
        server_options.tls.key_file = cert.key_path;
        server_options.tls.alpn = {"h2"};
        std::promise<std::uint16_t> port_promise;
        std::promise<fiber::http::HttpServer *> server_promise;
        auto port_future = port_promise.get_future();
        auto server_future = server_promise.get_future();
        fiber::async::spawn(loop, [&]() {
            return start_http_server(&loop, make_grpc_handler(), std::move(server_options), &port_promise,
                                     &server_promise);
        });
        server = server_future.get();
        port = port_future.get();
        return server != nullptr && port != 0;
    }
    void stop(fiber::event::EventLoop &loop) {
        if (server) {
            std::promise<void> done;
            auto future = done.get_future();
            fiber::async::spawn(loop, [&]() { return close_server_on_loop(server, &done); });
            future.get();
        }
    }
};

Result run_scenario(Scenario sc) {
    ServerCtx server;
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto &loop = group.at(0);

    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    if (server.start(loop)) {
        fiber::async::spawn(loop, [&]() { return run_client(&loop, server.port, sc, promise); });
    } else {
        Result r;
        r.err = fiber::common::IoErr::Unknown;
        promise->set_value(std::move(r));
    }

    Result result = future.get();
    server.stop(loop);
    group.stop();
    group.join();
    delete server.server;
    return result;
}

TEST(GrpcStreamTest, ServerStreaming) {
    const Result r = run_scenario(Scenario::ServerStream);
    ASSERT_EQ(r.err, fiber::common::IoErr::None) << "transport error";
    ASSERT_TRUE(r.finish_ok);
    ASSERT_TRUE(r.status.ok());
    ASSERT_EQ(r.status.code, 0);
    ASSERT_EQ(r.messages.size(), 3u);
    EXPECT_EQ(r.messages[0], "stream 0");
    EXPECT_EQ(r.messages[1], "stream 1");
    EXPECT_EQ(r.messages[2], "stream 2");
    ASSERT_EQ(r.counts.size(), 3u);
    EXPECT_EQ(r.counts[0], 0);
    EXPECT_EQ(r.counts[1], 1);
    EXPECT_EQ(r.counts[2], 2);
}

TEST(GrpcStreamTest, ClientStreaming) {
    const Result r = run_scenario(Scenario::ClientStream);
    ASSERT_EQ(r.err, fiber::common::IoErr::None) << "transport error";
    ASSERT_TRUE(r.finish_ok);
    ASSERT_TRUE(r.status.ok());
    ASSERT_EQ(r.messages.size(), 1u);
    EXPECT_EQ(r.messages[0], "sum");
    ASSERT_EQ(r.counts.size(), 1u);
    EXPECT_EQ(r.counts[0], 3); // server counted the 3 requests
}

TEST(GrpcStreamTest, BidiStreaming) {
    const Result r = run_scenario(Scenario::Bidi);
    ASSERT_EQ(r.err, fiber::common::IoErr::None) << "transport error";
    ASSERT_TRUE(r.finish_ok);
    ASSERT_TRUE(r.status.ok());
    ASSERT_EQ(r.messages.size(), 3u);
    EXPECT_EQ(r.messages[0], "c0");
    EXPECT_EQ(r.messages[1], "c1");
    EXPECT_EQ(r.messages[2], "c2");
}

TEST(GrpcStreamTest, TrailersOnlyError) {
    const Result r = run_scenario(Scenario::TrailersOnlyError);
    ASSERT_EQ(r.err, fiber::common::IoErr::None) << "transport error";
    ASSERT_TRUE(r.finish_ok);
    EXPECT_FALSE(r.status.ok());
    EXPECT_EQ(r.status.code, 5);
    EXPECT_EQ(r.status.message, "stream failure");
    EXPECT_TRUE(r.messages.empty());
}

TEST(GrpcStreamTest, CancelMidStream) {
    const Result r = run_scenario(Scenario::Cancel);
    EXPECT_EQ(r.err, fiber::common::IoErr::Canceled);
    EXPECT_FALSE(r.finish_ok);
}

TEST(GrpcStreamTest, DeadlineEnforcedLocally) {
    // With Options::deadline set, a stalled server must cause read() to fail with
    // TimedOut (the deadline is enforced locally, not just sent to the server).
    const Result r = run_scenario(Scenario::Deadline);
    EXPECT_EQ(r.err, fiber::common::IoErr::TimedOut);
    EXPECT_FALSE(r.finish_ok);
}

TEST(GrpcStreamTest, OversizeGrpcMessageTruncated) {
    // An untrusted grpc-message larger than the client cap must be truncated
    // rather than copied whole (bounds the allocation on a noexcept path).
    const Result r = run_scenario(Scenario::OversizeMessage);
    ASSERT_EQ(r.err, fiber::common::IoErr::None);
    ASSERT_TRUE(r.finish_ok);
    EXPECT_EQ(r.status.code, 5);
    constexpr std::size_t kCap = 8 * 1024;
    EXPECT_EQ(r.status.message.size(), kCap);
    EXPECT_EQ(r.status.message, std::string(kCap, 'x'));
}

} // namespace
