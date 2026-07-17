#include <gtest/gtest.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/socket.h>

#include <async/Sleep.h>
#include <async/Spawn.h>
#include <async/Timeout.h>
#include <async/WaitGroup.h>
#include <event/EventLoopGroup.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <grpc/GrpcFraming.h>
#include <grpc/ProtoCodec.h>
#include <http/Http2Connection.h>
#include <http/Http2HpackEncodeCatalog.h>
#include <http/HttpBodySpec.h>
#include <http/HttpExchange.h>
#include <http/HttpHeaders.h>
#include <http/HttpTransport.h>
#include <http/ServerRequestFactory.h>
#include <net/SocketAddress.h>
#include <net/TcpListener.h>

#include "../src/rpc/NacosGrpcConnection.h"

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;
namespace dto = fiber::nacos::dto;
namespace nacos_detail = fiber::nacos::detail;
namespace proto = fiber::nacos::proto;

fiber::common::IoResult<std::uint16_t> local_port(int fd) {
    sockaddr_storage storage{};
    socklen_t len = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &len) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&storage), len, address)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return address.port();
}

std::string grpc_frame(const google::protobuf::MessageLite &message) {
    std::string bytes;
    EXPECT_TRUE(message.SerializeToString(&bytes));
    const std::uint32_t size = static_cast<std::uint32_t>(bytes.size());
    std::string result;
    result.reserve(bytes.size() + 5);
    result.push_back('\0');
    result.push_back(static_cast<char>(size >> 24));
    result.push_back(static_cast<char>(size >> 16));
    result.push_back(static_cast<char>(size >> 8));
    result.push_back(static_cast<char>(size));
    result.append(bytes);
    return result;
}

fiber::async::Task<fiber::common::IoResult<proto::Payload>> read_payload(fiber::http::HttpExchange &exchange,
                                                                         fiber::grpc::GrpcFrameReader &reader) {
    for (;;) {
        fiber::mem::IoBufChain message;
        auto next = reader.next_payload(message);
        if (!next) {
            co_return std::unexpected(next.error());
        }
        if (*next) {
            proto::Payload payload;
            auto decoded = fiber::grpc::decode(message, payload);
            if (!decoded) {
                co_return std::unexpected(decoded.error());
            }
            co_return payload;
        }
        auto chunk = co_await exchange.read_body(64 * 1024, 2s);
        if (!chunk) {
            co_return std::unexpected(chunk.error());
        }
        const bool ended = chunk->complete();
        if (chunk->readable_bytes() > 0) {
            auto appended = reader.append(std::move(*chunk));
            if (!appended) {
                co_return std::unexpected(appended.error());
            }
        }
        if (ended && reader.buffered_bytes() == 0) {
            co_return std::unexpected(fiber::common::IoErr::NotFound);
        }
    }
}

fiber::async::Task<fiber::common::IoResult<void>> send_response_header(fiber::http::HttpExchange &exchange) {
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("content-type", "application/grpc");
    co_return co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::None(),
            .end_stream = false,
    });
}

fiber::async::Task<fiber::common::IoResult<void>> send_payload(fiber::http::HttpExchange &exchange,
                                                               const proto::Payload &payload) {
    const std::string framed = grpc_frame(payload);
    auto result = co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(framed.data()), framed.size(),
                                               false, 2s);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return fiber::common::IoResult<void>{};
}

fiber::async::Task<void> send_trailers(fiber::http::HttpExchange &exchange) {
    fiber::http::HttpHeaders trailers(exchange.pool());
    trailers.set("grpc-status", "0");
    (void) co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Trailer,
            .headers = &trailers,
            .body = fiber::http::HttpBodySpec::None(),
            .end_stream = true,
    });
}

class ScriptedNacosGrpcServer {
public:
    ScriptedNacosGrpcServer(fiber::event::EventLoop &loop, bool support_ability_negotiation,
                            bool reconnect_once = false) :
        loop_(&loop), support_ability_negotiation_(support_ability_negotiation),
        connections_to_serve_(reconnect_once ? 2 : 1), reconnect_once_(reconnect_once),
        handler_([this](fiber::http::HttpExchange &exchange) { return handle(exchange); }),
        factory_(http_options_, handler_), listener_(loop) {
        FIBER_ASSERT(catalog_.init({}));
        auto bound = listener_.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), {});
        if (!bound) {
            return;
        }
        auto port = local_port(listener_.fd());
        if (port) {
            port_ = *port;
        }
    }

    [[nodiscard]] bool valid() const noexcept { return port_ != 0; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    DetachedTask serve(std::shared_ptr<std::promise<void>> finished) {
        for (std::size_t i = 0; i < connections_to_serve_; ++i) {
            auto accepted = co_await listener_.accept();
            if (!accepted) {
                break;
            }
            auto transport = fiber::http::TcpTransport::create(*loop_, std::move(*accepted));
            if (!transport) {
                break;
            }
            fiber::http::Http2Connection::Options options;
            options.role = fiber::http::Http2Connection::ConnectionRole::Server;
            options.outbound_hpack_catalog = &catalog_;
            fiber::http::Http2Connection connection(options, &factory_, fiber::http::ServerRequestFactory::ops());
            active_connection_ = &connection;
            if (connection.start(std::move(*transport)) == fiber::common::IoErr::None) {
                (void) co_await connection.run();
            }
            active_connection_ = nullptr;
        }
        finished->set_value();
    }

    void close() noexcept {
        listener_.close();
        if (active_connection_) {
            active_connection_->shutdown();
        }
    }

    [[nodiscard]] bool setup_seen() const noexcept { return setup_seen_.load(std::memory_order_acquire); }
    [[nodiscard]] bool detection_acked() const noexcept { return detection_acked_.load(std::memory_order_acquire); }
    [[nodiscard]] bool unknown_error_acked() const noexcept {
        return unknown_error_acked_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool refreshed_token_seen() const noexcept {
        return refreshed_token_seen_.load(std::memory_order_acquire);
    }

private:
    nacos_detail::NacosPayloadMetadata server_metadata() const noexcept {
        return {
                .client_ip = "127.0.0.1",
                .client_version = "server",
                .namespace_id = "namespace",
                .access_token = "server-token",
        };
    }

    fiber::async::Task<void> handle_unary(fiber::http::HttpExchange &exchange) {
        fiber::grpc::GrpcFrameReader reader(1024 * 1024);
        auto request = co_await read_payload(exchange, reader);
        if (!request) {
            co_return;
        }
        auto view = nacos_detail::validate_payload(*request, 1024 * 1024);
        if (!view) {
            co_return;
        }
        auto headers = request->metadata().headers();
        if (view->type == dto::req::HealthCheckRequest::kTypeName) {
            const auto token = headers.find("accessToken");
            if (token != headers.end() && token->second == "token-2") {
                refreshed_token_seen_.store(true, std::memory_order_release);
            }
        }

        if (!(co_await send_response_header(exchange))) {
            co_return;
        }
        if (view->type == dto::req::ServerCheckRequest::kTypeName) {
            dto::resp::ServerCheckResponse response;
            response.connection_id.set_present("connection-1");
            response.support_ability_negotiation = support_ability_negotiation_;
            auto payload = nacos_detail::encode_payload(response, server_metadata(), 1024 * 1024);
            if (!payload || !(co_await send_payload(exchange, *payload))) {
                co_return;
            }
        } else if (view->type == dto::req::HealthCheckRequest::kTypeName) {
            auto payload =
                    nacos_detail::encode_payload(dto::resp::HealthCheckResponse{}, server_metadata(), 1024 * 1024);
            if (!payload || !(co_await send_payload(exchange, *payload))) {
                co_return;
            }
        } else {
            co_return;
        }
        co_await send_trailers(exchange);
    }

    fiber::async::Task<void> handle_bidi(fiber::http::HttpExchange &exchange) {
        fiber::grpc::GrpcFrameReader reader(1024 * 1024);
        auto setup_payload = co_await read_payload(exchange, reader);
        if (!setup_payload) {
            co_return;
        }
        auto setup_view = nacos_detail::validate_payload(*setup_payload, 1024 * 1024);
        if (!setup_view || setup_view->type != dto::req::ConnectionSetupRequest::kTypeName) {
            co_return;
        }
        fiber::mem::BufPool setup_pool;
        dto::req::ConnectionSetupRequest setup;
        if (!nacos_detail::parse_payload_json(*setup_view, setup_pool, setup)) {
            co_return;
        }
        if (!setup.labels.is_present() || !setup.ability_table.is_present()) {
            co_return;
        }
        const std::size_t setup_generation = setup_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
        setup_seen_.store(true, std::memory_order_release);

        if (!(co_await send_response_header(exchange))) {
            co_return;
        }
        if (support_ability_negotiation_) {
            auto ack = nacos_detail::encode_payload(dto::req::SetupAckRequest{}, server_metadata(), 1024 * 1024);
            if (!ack || !(co_await send_payload(exchange, *ack))) {
                co_return;
            }
        }

        auto unknown = nacos_detail::encode_payload_json(
                "FutureRequest", R"({"requestId":"future-1","module":"future"})", server_metadata(), 1024 * 1024);
        if (!unknown || !(co_await send_payload(exchange, *unknown))) {
            co_return;
        }

        dto::req::ClientDetectionRequest detection;
        detection.request_id.set_present("detect-1");
        auto detection_payload = nacos_detail::encode_payload(detection, server_metadata(), 1024 * 1024);
        if (!detection_payload || !(co_await send_payload(exchange, *detection_payload))) {
            co_return;
        }

        auto unknown_response_payload = co_await read_payload(exchange, reader);
        if (!unknown_response_payload) {
            co_return;
        }
        fiber::mem::BufPool unknown_pool;
        dto::resp::ErrorResponse unknown_response;
        auto unknown_view = nacos_detail::validate_payload(*unknown_response_payload, 1024 * 1024);
        auto unknown_result =
                unknown_view ? nacos_detail::parse_payload_json(*unknown_view, unknown_pool, unknown_response)
                             : std::expected<void, nacos_detail::NacosRpcError>(std::unexpected(unknown_view.error()));
        if (unknown_result && unknown_response.request_id.is_present() &&
            unknown_response.request_id.value() == "future-1" && !unknown_response.success()) {
            unknown_error_acked_.store(true, std::memory_order_release);
        }

        auto detection_response_payload = co_await read_payload(exchange, reader);
        if (!detection_response_payload) {
            co_return;
        }
        fiber::mem::BufPool response_pool;
        dto::resp::ClientDetectionResponse response;
        auto response_result =
                nacos_detail::decode_payload(*detection_response_payload, 1024 * 1024, response_pool, response);
        if (response_result && response.request_id.is_present() && response.request_id.value() == "detect-1") {
            detection_acked_.store(true, std::memory_order_release);
        }

        if (reconnect_once_ && setup_generation == 1) {
            (void) exchange.abort(fiber::common::IoErr::ConnReset);
            co_return;
        }

        for (;;) {
            auto chunk = co_await exchange.read_body(64 * 1024, 2s);
            if (!chunk || chunk->complete()) {
                break;
            }
        }
    }

    fiber::async::Task<void> handle(fiber::http::HttpExchange &exchange) {
        if (exchange.uri().path == "/Request/request") {
            co_await handle_unary(exchange);
        } else if (exchange.uri().path == "/BiRequestStream/requestBiStream") {
            co_await handle_bidi(exchange);
        }
    }

    fiber::event::EventLoop *loop_ = nullptr;
    bool support_ability_negotiation_ = false;
    std::size_t connections_to_serve_ = 1;
    bool reconnect_once_ = false;
    fiber::http::HttpServerOptions http_options_;
    fiber::http::HttpHandler handler_;
    fiber::http::ServerRequestFactory factory_;
    fiber::http::Http2HpackEncodeCatalog catalog_;
    fiber::net::TcpListener listener_;
    fiber::http::Http2Connection *active_connection_ = nullptr;
    std::uint16_t port_ = 0;
    std::atomic<bool> setup_seen_{false};
    std::atomic<std::size_t> setup_count_{0};
    std::atomic<bool> unknown_error_acked_{false};
    std::atomic<bool> detection_acked_{false};
    std::atomic<bool> refreshed_token_seen_{false};
};

struct ConnectionResult {
    bool ready = false;
    bool stopped = false;
    bool setup_seen = false;
    bool unknown_error_acked = false;
    bool detection_acked = false;
    bool refreshed_token_seen = false;
};

DetachedTask drive_connection(nacos_detail::NacosGrpcConnection *connection, fiber::async::WaitGroup *group) {
    co_await connection->run();
    group->done();
}

DetachedTask run_connection_case(fiber::event::EventLoop *loop, ScriptedNacosGrpcServer *server,
                                 fiber::nacos::NacosClientConfig config, fiber::nacos::NacosClientOptions options,
                                 std::uint64_t minimum_generation,
                                 std::shared_ptr<std::promise<ConnectionResult>> finished) {
    ConnectionResult result;
    nacos_detail::NacosGrpcConnection connection(*loop, config, options);
    auto states = connection.subscribe_state();
    std::uint64_t state_version = states.current().version;

    fiber::nacos::NacosAuthSnapshot auth;
    auth.state = fiber::nacos::NacosAuthState::Ready;
    auth.access_token = "token-1";
    auth.expires_at = loop->now() + 1min;
    auth.generation = 1;
    connection.notify_auth(auth);

    fiber::async::WaitGroup run_group;
    run_group.add();
    fiber::async::spawn(*loop, [&connection, &run_group]() { return drive_connection(&connection, &run_group); });

    for (;;) {
        auto next = co_await fiber::async::timeout_for(
                [&states, state_version]() { return states.next(state_version); }, 2s);
        if (!next) {
            break;
        }
        state_version = next->version;
        if (next->value->state == nacos_detail::NacosGrpcConnectionState::Ready &&
            next->value->generation >= minimum_generation) {
            result.ready = true;
            break;
        }
        if (next->value->state == nacos_detail::NacosGrpcConnectionState::Stopped) {
            break;
        }
    }

    if (result.ready) {
        auth.access_token = "token-2";
        auth.generation = 2;
        connection.notify_auth(auth);
        for (int i = 0; i < 200 && (!server->unknown_error_acked() || !server->detection_acked() ||
                                    !server->refreshed_token_seen());
             ++i) {
            co_await fiber::async::sleep(5ms);
        }
    }

    connection.shutdown();
    co_await run_group.join();
    auto stopped = states.current();
    result.stopped = stopped.value && stopped.value->state == nacos_detail::NacosGrpcConnectionState::Stopped;
    result.setup_seen = server->setup_seen();
    result.unknown_error_acked = server->unknown_error_acked();
    result.detection_acked = server->detection_acked();
    result.refreshed_token_seen = server->refreshed_token_seen();
    finished->set_value(result);
}

ConnectionResult execute_connection_case(bool support_ability_negotiation, bool reconnect_once = false) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    auto server = std::make_unique<ScriptedNacosGrpcServer>(group.at(0), support_ability_negotiation, reconnect_once);
    EXPECT_TRUE(server->valid());
    auto server_finished = std::make_shared<std::promise<void>>();
    auto server_future = server_finished->get_future();
    fiber::async::spawn(group.at(0),
                        [server_ptr = server.get(), server_finished]() { return server_ptr->serve(server_finished); });

    fiber::nacos::NacosClientConfigParams params;
    params.server_ips.push_back(fiber::net::IpAddress::v4({127, 0, 0, 2}));
    params.server_ips.push_back(fiber::net::IpAddress::loopback_v4());
    params.username = "nacos";
    params.password = "nacos";
    params.grpc_port = server->port();
    params.namespace_id = "namespace";
    params.tenant = "tenant";
    auto config = fiber::nacos::NacosClientConfig::create(std::move(params));
    EXPECT_TRUE(config.has_value());

    fiber::nacos::NacosClientOptions options;
    options.grpc_connect_timeout = 500ms;
    options.grpc_request_timeout = 500ms;
    options.grpc_handshake_timeout = 1s;
    options.grpc_compatibility_setup_delay = 20ms;
    options.grpc_heartbeat_interval = 20ms;
    options.grpc_reconnect_initial_delay = 20ms;
    options.grpc_reconnect_max_delay = 100ms;
    options.max_inbound_grpc_message_bytes = 1024 * 1024;
    options.max_push_response_bytes = 64 * 1024;

    auto client_finished = std::make_shared<std::promise<ConnectionResult>>();
    auto client_future = client_finished->get_future();
    fiber::async::spawn(group.at(0), [loop = &group.at(0), server_ptr = server.get(), config = std::move(*config),
                                      options, reconnect_once, client_finished]() mutable {
        return run_connection_case(loop, server_ptr, std::move(config), options, reconnect_once ? 2 : 1,
                                   client_finished);
    });

    ConnectionResult result;
    if (client_future.wait_for(5s) == std::future_status::ready) {
        result = client_future.get();
    } else {
        ADD_FAILURE() << "timed out waiting for Nacos gRPC connection";
    }

    auto close_finished = std::make_shared<std::promise<void>>();
    auto close_future = close_finished->get_future();
    fiber::async::spawn(group.at(0), [server_ptr = server.get(), close_finished]() -> DetachedTask {
        server_ptr->close();
        close_finished->set_value();
        co_return;
    });
    (void) close_future.wait_for(1s);
    (void) server_future.wait_for(1s);
    group.stop();
    group.join();
    return result;
}

TEST(NacosGrpcConnectionTest, NegotiatesSetupAckHandlesPushRefreshesTokenAndStopsCleanly) {
    const ConnectionResult result = execute_connection_case(true);
    EXPECT_TRUE(result.ready);
    EXPECT_TRUE(result.stopped);
    EXPECT_TRUE(result.setup_seen);
    EXPECT_TRUE(result.unknown_error_acked);
    EXPECT_TRUE(result.detection_acked);
    EXPECT_TRUE(result.refreshed_token_seen);
}

TEST(NacosGrpcConnectionTest, UsesCompatibilityDelayWhenServerDoesNotNegotiateAbilities) {
    const ConnectionResult result = execute_connection_case(false);
    EXPECT_TRUE(result.ready);
    EXPECT_TRUE(result.stopped);
    EXPECT_TRUE(result.setup_seen);
    EXPECT_TRUE(result.unknown_error_acked);
    EXPECT_TRUE(result.detection_acked);
    EXPECT_TRUE(result.refreshed_token_seen);
}

TEST(NacosGrpcConnectionTest, ReconnectsAfterReadyBidirectionalStreamReset) {
    const ConnectionResult result = execute_connection_case(true, true);
    EXPECT_TRUE(result.ready);
    EXPECT_TRUE(result.stopped);
    EXPECT_TRUE(result.setup_seen);
    EXPECT_TRUE(result.unknown_error_acked);
    EXPECT_TRUE(result.detection_acked);
    EXPECT_TRUE(result.refreshed_token_seen);
}

DetachedTask run_rnacos_interop(fiber::event::EventLoop *loop, fiber::nacos::NacosClientConfig config,
                                fiber::nacos::NacosClientOptions options,
                                std::shared_ptr<std::promise<ConnectionResult>> finished) {
    ConnectionResult result;
    nacos_detail::NacosGrpcConnection connection(*loop, config, options);
    auto states = connection.subscribe_state();
    std::uint64_t state_version = states.current().version;

    fiber::nacos::NacosAuthSnapshot auth;
    auth.state = fiber::nacos::NacosAuthState::Ready;
    auth.access_token = "rnacos-integration-token";
    auth.expires_at = loop->now() + 1min;
    connection.notify_auth(auth);

    fiber::async::WaitGroup run_group;
    run_group.add();
    fiber::async::spawn(*loop, [&connection, &run_group]() { return drive_connection(&connection, &run_group); });
    for (;;) {
        auto next = co_await fiber::async::timeout_for(
                [&states, state_version]() { return states.next(state_version); }, 3s);
        if (!next) {
            break;
        }
        state_version = next->version;
        if (next->value->state == nacos_detail::NacosGrpcConnectionState::Ready) {
            result.ready = true;
            break;
        }
    }
    connection.shutdown();
    co_await run_group.join();
    const auto stopped = states.current();
    result.stopped = stopped.value && stopped.value->state == nacos_detail::NacosGrpcConnectionState::Stopped;
    finished->set_value(result);
}

TEST(NacosGrpcConnectionTest, RnacosInteropWhenEnabled) {
    const char *port_text = std::getenv("FIBER_NACOS_TEST_GRPC_PORT");
    if (!port_text) {
        GTEST_SKIP() << "set FIBER_NACOS_TEST_GRPC_PORT to run rnacos interoperability";
    }
    unsigned port = 0;
    const std::string_view port_view(port_text);
    const auto parsed = std::from_chars(port_view.data(), port_view.data() + port_view.size(), port);
    ASSERT_EQ(parsed.ec, std::errc());
    ASSERT_EQ(parsed.ptr, port_view.data() + port_view.size());
    ASSERT_GT(port, 0u);
    ASSERT_LE(port, 65535u);

    fiber::nacos::NacosClientConfigParams params;
    params.server_ips.push_back(fiber::net::IpAddress::loopback_v4());
    params.username = "nacos";
    params.password = "nacos";
    params.grpc_port = static_cast<std::uint16_t>(port);
    auto config = fiber::nacos::NacosClientConfig::create(std::move(params));
    ASSERT_TRUE(config.has_value());

    fiber::nacos::NacosClientOptions options;
    options.grpc_connect_timeout = 1s;
    options.grpc_request_timeout = 1s;
    options.grpc_handshake_timeout = 2s;
    options.grpc_compatibility_setup_delay = 50ms;
    options.grpc_heartbeat_interval = 100ms;
    options.grpc_reconnect_initial_delay = 20ms;
    options.grpc_reconnect_max_delay = 100ms;

    fiber::event::EventLoopGroup group(1);
    group.start();
    auto finished = std::make_shared<std::promise<ConnectionResult>>();
    auto future = finished->get_future();
    fiber::async::spawn(group.at(0), [loop = &group.at(0), config = std::move(*config), options, finished]() mutable {
        return run_rnacos_interop(loop, std::move(config), options, finished);
    });
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    const ConnectionResult result = future.get();
    group.stop();
    group.join();
    EXPECT_TRUE(result.ready);
    EXPECT_TRUE(result.stopped);
}

} // namespace
