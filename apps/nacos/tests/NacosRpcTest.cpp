#include <gtest/gtest.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <sys/socket.h>

#include <async/Sleep.h>
#include <async/Spawn.h>
#include <event/EventLoopGroup.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <http/Http2Connection.h>
#include <http/HttpBodySpec.h>
#include <http/HttpExchange.h>
#include <http/HttpHeaders.h>
#include <http/HttpTransport.h>
#include <http/ServerRequestFactory.h>
#include <net/SocketAddress.h>
#include <net/TcpListener.h>
#include "../src/dto/JsonCodec.h"
#include "rpc/grpc/GrpcFraming.h"
#include "rpc/grpc/ProtoCodec.h"

#include "../src/rpc/NacosRpc.h"

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

fiber::async::Task<fiber::common::IoResult<proto::Payload>>
read_payload(fiber::http::HttpExchange &exchange, fiber::nacos::detail::grpc::GrpcFrameReader &reader) {
    for (;;) {
        fiber::mem::IoBufChain message;
        auto next = reader.next_payload(message);
        if (!next) {
            co_return std::unexpected(next.error());
        }
        if (*next) {
            proto::Payload payload;
            auto decoded = fiber::nacos::detail::grpc::decode(message, payload);
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
    auto result = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(framed.data()), framed.size(),
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

class ScriptedNacosRpcServer {
public:
    ScriptedNacosRpcServer(fiber::event::EventLoop &loop, bool support_ability_negotiation,
                           bool send_connect_reset = false) :
        loop_(&loop), support_ability_negotiation_(support_ability_negotiation),
        send_connect_reset_(send_connect_reset),
        handler_([this](fiber::http::HttpExchange &exchange) { return handle(exchange); }),
        factory_(http_options_, handler_), listener_(loop) {
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
    [[nodiscard]] bool setup_valid() const noexcept { return setup_valid_.load(std::memory_order_acquire); }
    [[nodiscard]] bool handler_response_seen() const noexcept {
        return handler_response_seen_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool handler_error_seen() const noexcept {
        return handler_error_seen_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool detection_seen() const noexcept { return detection_seen_.load(std::memory_order_acquire); }
    [[nodiscard]] bool unknown_seen() const noexcept { return unknown_seen_.load(std::memory_order_acquire); }
    [[nodiscard]] bool refreshed_token_seen() const noexcept {
        return refreshed_token_seen_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool connect_reset_seen() const noexcept {
        return connect_reset_seen_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool connect_reset_enabled() const noexcept { return send_connect_reset_; }

    DetachedTask serve(std::shared_ptr<std::promise<void>> finished) {
        auto accepted = co_await listener_.accept();
        if (accepted) {
            auto transport = fiber::http::TcpTransport::create(*loop_, std::move(*accepted), http_options_.tcp);
            if (transport) {
                fiber::http::Http2Connection::Options options;
                options.role = fiber::http::Http2Connection::ConnectionRole::Server;
                fiber::http::Http2Connection connection(options, &factory_, fiber::http::ServerRequestFactory::ops());
                active_connection_ = &connection;
                if (connection.start(std::move(*transport)) == fiber::common::IoErr::None) {
                    (void) co_await connection.wait_closed();
                }
                active_connection_ = nullptr;
            }
        }
        finished->set_value();
    }

    void close() noexcept {
        listener_.close();
        if (active_connection_) {
            active_connection_->shutdown();
        }
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

    template<typename Response>
    bool decode_response(const proto::Payload &payload, Response &response, fiber::mem::BufPool &pool) {
        auto view = nacos_detail::validate_payload(payload, 1024 * 1024);
        return view && view->type == Response::kTypeName &&
               nacos_detail::parse_payload_json(*view, pool, response).has_value();
    }

    fiber::async::Task<void> handle_unary(fiber::http::HttpExchange &exchange) {
        fiber::nacos::detail::grpc::GrpcFrameReader reader(1024 * 1024);
        auto request = co_await read_payload(exchange, reader);
        if (!request) {
            co_return;
        }
        auto view = nacos_detail::validate_payload(*request, 1024 * 1024);
        if (!view || !(co_await send_response_header(exchange))) {
            co_return;
        }

        if (view->type == dto::req::ServerCheckRequest::kTypeName) {
            dto::resp::ServerCheckResponse response;
            response.connection_id.set_present("rpc-connection-1");
            response.support_ability_negotiation = support_ability_negotiation_;
            auto payload = nacos_detail::encode_payload(response, server_metadata(), 1024 * 1024);
            if (!payload || !(co_await send_payload(exchange, *payload))) {
                co_return;
            }
        } else if (view->type == dto::req::ConfigQueryRequest::kTypeName) {
            const auto &headers = request->metadata().headers();
            auto token = headers.find("accessToken");
            if (token != headers.end() && token->second == "token-2") {
                refreshed_token_seen_.store(true, std::memory_order_release);
            }
            dto::resp::ConfigQueryResponse response;
            response.content.set_present("value");
            response.md5.set_present("md5");
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

    fiber::async::Task<void> send_server_requests(fiber::http::HttpExchange &exchange) {
        dto::req::ConfigChangeNotifyRequest notify;
        notify.request_id.set_present("config-1");
        notify.data_id.set_present("data");
        notify.group.set_present("group");
        notify.tenant.set_present("tenant");
        auto notify_payload = nacos_detail::encode_payload(notify, server_metadata(), 1024 * 1024);
        if (!notify_payload) {
            co_return;
        }
        (*notify_payload->mutable_metadata()->mutable_headers())["serverKey"] = "serverValue";
        if (!(co_await send_payload(exchange, *notify_payload))) {
            co_return;
        }

        dto::req::ConfigChangeNotifyRequest failed;
        failed.request_id.set_present("config-fail");
        failed.data_id.set_present("fail");
        failed.group.set_present("group");
        failed.tenant.set_present("tenant");
        auto failed_payload = nacos_detail::encode_payload(failed, server_metadata(), 1024 * 1024);
        if (!failed_payload || !(co_await send_payload(exchange, *failed_payload))) {
            co_return;
        }

        dto::req::ClientDetectionRequest detection;
        detection.request_id.set_present("detect-1");
        auto detection_payload = nacos_detail::encode_payload(detection, server_metadata(), 1024 * 1024);
        if (!detection_payload || !(co_await send_payload(exchange, *detection_payload))) {
            co_return;
        }

        auto unknown = nacos_detail::encode_payload_json(
                "FutureRequest", R"({"requestId":"future-1","module":"future"})", server_metadata(), 1024 * 1024);
        if (!unknown || !(co_await send_payload(exchange, *unknown))) {
            co_return;
        }

        if (send_connect_reset_) {
            for (int i = 0; i < 200 && !refreshed_token_seen(); ++i) {
                co_await fiber::async::sleep(1ms);
            }
            dto::req::ConnectResetRequest reset;
            reset.request_id.set_present("reset-1");
            reset.server_ip.set_present("127.0.0.1");
            // server_port is Nullable<string_view> (non-owning); keep the text
            // alive across encode_payload, otherwise the std::to_string temporary
            // dangles and the encoded JSON contains garbage (use-after-free).
            std::string server_port_text = std::to_string(port_);
            reset.server_port.set_present(server_port_text);
            auto reset_payload = nacos_detail::encode_payload(reset, server_metadata(), 1024 * 1024);
            if (!reset_payload || !(co_await send_payload(exchange, *reset_payload))) {
                co_return;
            }
        }
    }

    fiber::async::Task<void> read_server_responses(fiber::http::HttpExchange &exchange,
                                                   fiber::nacos::detail::grpc::GrpcFrameReader &reader) {
        auto handled_payload = co_await read_payload(exchange, reader);
        if (!handled_payload) {
            co_return;
        }
        fiber::mem::BufPool handled_pool;
        dto::resp::ConfigChangeNotifyResponse handled;
        if (decode_response(*handled_payload, handled, handled_pool) && handled.request_id.is_present() &&
            handled.request_id.value() == "config-1" && handled.message.is_present() &&
            handled.message.value() == "handled") {
            handler_response_seen_.store(true, std::memory_order_release);
        }

        auto failed_payload = co_await read_payload(exchange, reader);
        if (!failed_payload) {
            co_return;
        }
        fiber::mem::BufPool failed_pool;
        dto::resp::ErrorResponse failed;
        if (decode_response(*failed_payload, failed, failed_pool) && failed.request_id.is_present() &&
            failed.request_id.value() == "config-fail" &&
            failed.error_code == fiber::common::io_err_to_errno(fiber::common::IoErr::Invalid) &&
            failed.message.is_present() && failed.message.value() == "invalid" && !failed.success()) {
            handler_error_seen_.store(true, std::memory_order_release);
        }

        auto detection_payload = co_await read_payload(exchange, reader);
        if (!detection_payload) {
            co_return;
        }
        fiber::mem::BufPool detection_pool;
        dto::resp::ClientDetectionResponse detection;
        if (decode_response(*detection_payload, detection, detection_pool) && detection.request_id.is_present() &&
            detection.request_id.value() == "detect-1") {
            detection_seen_.store(true, std::memory_order_release);
        }

        auto unknown_payload = co_await read_payload(exchange, reader);
        if (!unknown_payload) {
            co_return;
        }
        fiber::mem::BufPool unknown_pool;
        dto::resp::ErrorResponse unknown;
        if (decode_response(*unknown_payload, unknown, unknown_pool) && unknown.request_id.is_present() &&
            unknown.request_id.value() == "future-1" && !unknown.success()) {
            unknown_seen_.store(true, std::memory_order_release);
        }

        if (send_connect_reset_) {
            auto reset_payload = co_await read_payload(exchange, reader);
            if (!reset_payload) {
                co_return;
            }
            fiber::mem::BufPool reset_pool;
            dto::resp::ConnectResetResponse reset;
            if (decode_response(*reset_payload, reset, reset_pool) && reset.request_id.is_present() &&
                reset.request_id.value() == "reset-1") {
                connect_reset_seen_.store(true, std::memory_order_release);
            }
        }
    }

    fiber::async::Task<void> handle_bidi(fiber::http::HttpExchange &exchange) {
        fiber::nacos::detail::grpc::GrpcFrameReader reader(1024 * 1024);
        auto setup_payload = co_await read_payload(exchange, reader);
        if (!setup_payload) {
            co_return;
        }
        auto setup_view = nacos_detail::validate_payload(*setup_payload, 1024 * 1024);
        fiber::mem::BufPool setup_pool;
        dto::req::ConnectionSetupRequest setup;
        if (!setup_view || setup_view->type != dto::req::ConnectionSetupRequest::kTypeName ||
            !nacos_detail::parse_payload_json(*setup_view, setup_pool, setup)) {
            co_return;
        }
        const auto *module = setup.labels.is_present() ? setup.labels.value().find("module") : nullptr;
        const auto *source = setup.labels.is_present() ? setup.labels.value().find("source") : nullptr;
        if (module && module->value == "config" && source && source->value == "sdk" &&
            setup.ability_table.is_present()) {
            setup_valid_.store(true, std::memory_order_release);
        }

        if (!(co_await send_response_header(exchange))) {
            co_return;
        }
        if (support_ability_negotiation_) {
            auto ack = nacos_detail::encode_payload(dto::req::SetupAckRequest{}, server_metadata(), 1024 * 1024);
            if (!ack || !(co_await send_payload(exchange, *ack))) {
                co_return;
            }
        }
        co_await send_server_requests(exchange);
        co_await read_server_responses(exchange, reader);

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
    bool send_connect_reset_ = false;
    fiber::http::HttpServerOptions http_options_;
    fiber::http::HttpHandler handler_;
    fiber::http::ServerRequestFactory factory_;
    fiber::net::TcpListener listener_;
    fiber::http::Http2Connection *active_connection_ = nullptr;
    std::uint16_t port_ = 0;
    std::atomic<bool> setup_valid_{false};
    std::atomic<bool> handler_response_seen_{false};
    std::atomic<bool> handler_error_seen_{false};
    std::atomic<bool> detection_seen_{false};
    std::atomic<bool> unknown_seen_{false};
    std::atomic<bool> refreshed_token_seen_{false};
    std::atomic<bool> connect_reset_seen_{false};
};

struct HandlerContext {
    bool called = false;
    bool header_seen = false;
};

fiber::async::Task<fiber::common::IoResult<dto::resp::ConfigChangeNotifyResponse>>
handle_config_change(void *opaque, nacos_detail::NacosServerRequestContext &request_context,
                     const dto::req::ConfigChangeNotifyRequest &request) noexcept {
    auto *context = static_cast<HandlerContext *>(opaque);
    if (request.data_id.is_present() && request.data_id.value() == "fail") {
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }
    co_await fiber::async::sleep(1ms);
    context->called = true;
    auto header = request_context.headers().find("serverKey");
    context->header_seen = header && *header == "serverValue";
    auto message = request_context.copy_to_pool("handled");
    if (!message) {
        co_return std::unexpected(message.error());
    }
    dto::resp::ConfigChangeNotifyResponse response;
    response.message.set_present(*message);
    co_return response;
}

struct RpcCaseResult {
    bool started = false;
    bool stopped = false;
    bool handler_called = false;
    bool handler_header_seen = false;
    bool unary_response_valid = false;
    std::string connection_id;
    nacos_detail::NacosRpcCloseKind close_kind = nacos_detail::NacosRpcCloseKind::None;
    bool redirect_valid = false;
};

DetachedTask run_rpc_case(fiber::event::EventLoop *loop, ScriptedNacosRpcServer *server,
                          fiber::nacos::NacosClientConfig config, fiber::nacos::NacosRpcOptions options,
                          std::shared_ptr<std::promise<RpcCaseResult>> finished) {
    fiber::async::Watch<fiber::nacos::NacosAuthAccess> auth(fiber::nacos::NacosAuthAccess{
            .kind = fiber::nacos::NacosAuthAccessKind::Present,
            .access_token = "token-1",
    });
    auto auth_publisher = auth.acquire_publisher();
    EXPECT_TRUE(auth_publisher.has_value());
    auto auth_subscriber = auth.subscribe();

    nacos_detail::NacosRpc rpc(
            nacos_detail::NacosRpcDependencies{
                    .loop = *loop,
                    .config = config,
                    .options = options,
                    .auth = auth_subscriber,
            },
            nacos_detail::NacosRpcEndpoint{
                    .ip = fiber::net::IpAddress::loopback_v4(),
                    .port = server->port(),
                    .server_index = 0,
            },
            nacos_detail::NacosRpcModule::Config);

    nacos_detail::NacosBiRequestHandler handlers;
    HandlerContext handler_context;
    auto registered =
            handlers.add_request_handler<dto::req::ConfigChangeNotifyRequest, dto::resp::ConfigChangeNotifyResponse>(
                    &handle_config_change, &handler_context);
    EXPECT_TRUE(registered.has_value());
    auto duplicate =
            handlers.add_request_handler<dto::req::ConfigChangeNotifyRequest, dto::resp::ConfigChangeNotifyResponse>(
                    &handle_config_change, &handler_context);
    EXPECT_EQ(duplicate.error(), nacos_detail::NacosHandlerRegistrationError::DuplicateType);

    RpcCaseResult result;
    fiber::async::WaitGroup run_done;
    run_done.add();
    nacos_detail::NacosRpcCloseResult close_result;
    fiber::async::spawn(*loop, [&]() -> DetachedTask {
        close_result = co_await rpc.run(handlers);
        run_done.done();
    });
    auto ready = co_await rpc.wait_ready();
    result.started = ready.has_value();
    result.connection_id = rpc.connection_id();

    if (ready) {
        auth_publisher->publish(fiber::nacos::NacosAuthAccess{
                .kind = fiber::nacos::NacosAuthAccessKind::Present,
                .access_token = "token-2",
        });
        dto::req::ConfigQueryRequest request;
        request.data_id.set_present("data");
        request.group.set_present("group");
        request.tenant.set_present("tenant");
        dto::resp::ConfigQueryResponse response;
        fiber::mem::BufPool pool;
        auto queried = co_await rpc.request(request, pool, response);
        result.unary_response_valid = queried && response.content.is_present() && response.content.value() == "value";

        for (int i = 0;
             i < 200 && (!server->handler_response_seen() || !server->handler_error_seen() ||
                         !server->detection_seen() || !server->unknown_seen() || !server->refreshed_token_seen() ||
                         (server->connect_reset_enabled() && !server->connect_reset_seen()));
             ++i) {
            co_await fiber::async::sleep(5ms);
        }
    }

    result.handler_called = handler_context.called;
    result.handler_header_seen = handler_context.header_seen;
    rpc.shutdown();
    co_await run_done.join();
    result.stopped = rpc.state() == nacos_detail::NacosRpcState::Stopped;
    result.close_kind = close_result.kind;
    result.redirect_valid =
            close_result.redirect && close_result.redirect->ip == fiber::net::IpAddress::loopback_v4() &&
            close_result.redirect->port == server->port() && !close_result.redirect->server_index.has_value();
    finished->set_value(std::move(result));
}

RpcCaseResult execute_rpc_case(bool support_ability_negotiation, bool send_connect_reset = false) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto server =
            std::make_unique<ScriptedNacosRpcServer>(group.at(0), support_ability_negotiation, send_connect_reset);
    EXPECT_TRUE(server->valid());
    auto server_finished = std::make_shared<std::promise<void>>();
    auto server_future = server_finished->get_future();
    fiber::async::spawn(group.at(0),
                        [server_ptr = server.get(), server_finished]() { return server_ptr->serve(server_finished); });

    fiber::nacos::NacosClientConfigParams params;
    params.server_ips.push_back(fiber::net::IpAddress::loopback_v4());
    params.username = "nacos";
    params.password = "nacos";
    params.grpc_port = server->port();
    params.namespace_id = "namespace";
    params.tenant = "tenant";
    auto config = fiber::nacos::NacosClientConfig::create(std::move(params));
    EXPECT_TRUE(config.has_value());

    fiber::nacos::NacosRpcOptions options;
    options.connect_timeout = 500ms;
    options.request_timeout = 500ms;
    options.handshake_timeout = 1s;
    options.compatibility_setup_delay = 20ms;
    options.heartbeat_interval = 100ms;
    options.max_inbound_message_bytes = 1024 * 1024;

    auto client_finished = std::make_shared<std::promise<RpcCaseResult>>();
    auto client_future = client_finished->get_future();
    fiber::async::spawn(group.at(0), [loop = &group.at(0), server_ptr = server.get(), config = std::move(*config),
                                      options, client_finished]() mutable {
        return run_rpc_case(loop, server_ptr, std::move(config), options, client_finished);
    });

    RpcCaseResult result;
    if (client_future.wait_for(5s) == std::future_status::ready) {
        result = client_future.get();
    } else {
        ADD_FAILURE() << "timed out waiting for NacosRpc case";
    }

    const bool setup_valid = server->setup_valid();
    const bool handler_response_seen = server->handler_response_seen();
    const bool handler_error_seen = server->handler_error_seen();
    const bool detection_seen = server->detection_seen();
    const bool unknown_seen = server->unknown_seen();
    const bool refreshed_token_seen = server->refreshed_token_seen();
    const bool connect_reset_seen = server->connect_reset_seen();
    EXPECT_TRUE(setup_valid);
    EXPECT_TRUE(handler_response_seen);
    EXPECT_TRUE(handler_error_seen);
    EXPECT_TRUE(detection_seen);
    EXPECT_TRUE(unknown_seen);
    EXPECT_TRUE(refreshed_token_seen);
    EXPECT_EQ(connect_reset_seen, send_connect_reset);

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

TEST(NacosRpcTest, TypedHandlersUnaryRequestsAndSetupAckWork) {
    const RpcCaseResult result = execute_rpc_case(true);
    EXPECT_TRUE(result.started);
    EXPECT_TRUE(result.stopped);
    EXPECT_TRUE(result.handler_called);
    EXPECT_TRUE(result.handler_header_seen);
    EXPECT_TRUE(result.unary_response_valid);
    EXPECT_EQ(result.connection_id, "rpc-connection-1");
    EXPECT_EQ(result.close_kind, nacos_detail::NacosRpcCloseKind::Shutdown);
}

TEST(NacosRpcTest, CompatibilityDelayStartsTheSameTypedDispatcher) {
    const RpcCaseResult result = execute_rpc_case(false);
    EXPECT_TRUE(result.started);
    EXPECT_TRUE(result.stopped);
    EXPECT_TRUE(result.handler_called);
    EXPECT_TRUE(result.unary_response_valid);
}

TEST(NacosRpcTest, ConnectResetReturnsRedirectAfterResponseIsWritten) {
    const RpcCaseResult result = execute_rpc_case(true, true);
    EXPECT_TRUE(result.started);
    EXPECT_TRUE(result.stopped);
    EXPECT_EQ(result.close_kind, nacos_detail::NacosRpcCloseKind::Redirect);
    EXPECT_TRUE(result.redirect_valid);
}

struct PreRunShutdownResult {
    bool ready_canceled = false;
    bool stopped = false;
    nacos_detail::NacosRpcCloseKind close_kind = nacos_detail::NacosRpcCloseKind::None;
};

DetachedTask run_pre_run_shutdown(fiber::event::EventLoop *loop, fiber::nacos::NacosClientConfig config,
                                  fiber::nacos::NacosRpcOptions options,
                                  std::shared_ptr<std::promise<PreRunShutdownResult>> finished) {
    fiber::async::Watch<fiber::nacos::NacosAuthAccess> auth(fiber::nacos::NacosAuthAccess{
            .kind = fiber::nacos::NacosAuthAccessKind::NotConfigured,
    });
    auto auth_subscriber = auth.subscribe();
    nacos_detail::NacosRpc rpc(
            nacos_detail::NacosRpcDependencies{
                    .loop = *loop,
                    .config = config,
                    .options = options,
                    .auth = auth_subscriber,
            },
            nacos_detail::NacosRpcEndpoint{
                    .ip = fiber::net::IpAddress::loopback_v4(),
                    .port = config.grpc_port(),
                    .server_index = 0,
            },
            nacos_detail::NacosRpcModule::Config);
    nacos_detail::NacosBiRequestHandler handlers;

    rpc.shutdown();
    auto ready = co_await rpc.wait_ready();
    auto closed = co_await rpc.run(handlers);
    finished->set_value(PreRunShutdownResult{
            .ready_canceled = !ready && ready.error().code == nacos_detail::NacosRpcErrorCode::Shutdown,
            .stopped = rpc.state() == nacos_detail::NacosRpcState::Stopped,
            .close_kind = closed.kind,
    });
}

TEST(NacosRpcTest, ShutdownBeforeRunCompletesWithoutConnecting) {
    fiber::nacos::NacosClientConfigParams params;
    params.server_ips.push_back(fiber::net::IpAddress::loopback_v4());
    params.grpc_port = 1;
    auto config = fiber::nacos::NacosClientConfig::create(std::move(params));
    ASSERT_TRUE(config.has_value());

    fiber::event::EventLoopGroup group(1);
    group.start();
    auto finished = std::make_shared<std::promise<PreRunShutdownResult>>();
    auto future = finished->get_future();
    fiber::async::spawn(group.at(0), [loop = &group.at(0), config = std::move(*config),
                                      options = fiber::nacos::NacosRpcOptions{}, finished]() mutable {
        return run_pre_run_shutdown(loop, std::move(config), options, finished);
    });
    ASSERT_EQ(future.wait_for(1s), std::future_status::ready);
    const PreRunShutdownResult result = future.get();
    group.stop();
    group.join();

    EXPECT_TRUE(result.ready_canceled);
    EXPECT_TRUE(result.stopped);
    EXPECT_EQ(result.close_kind, nacos_detail::NacosRpcCloseKind::Shutdown);
}

DetachedTask run_rnacos_rpc(fiber::event::EventLoop *loop, fiber::nacos::NacosClientConfig config,
                            fiber::nacos::NacosRpcOptions options,
                            std::shared_ptr<std::promise<RpcCaseResult>> finished,
                            std::shared_ptr<std::atomic<int>> stage) {
    fiber::async::Watch<fiber::nacos::NacosAuthAccess> auth(fiber::nacos::NacosAuthAccess{
            .kind = fiber::nacos::NacosAuthAccessKind::NotConfigured,
    });
    auto auth_subscriber = auth.subscribe();
    nacos_detail::NacosRpc rpc(
            nacos_detail::NacosRpcDependencies{
                    .loop = *loop,
                    .config = config,
                    .options = options,
                    .auth = auth_subscriber,
            },
            nacos_detail::NacosRpcEndpoint{
                    .ip = fiber::net::IpAddress::loopback_v4(),
                    .port = config.grpc_port(),
                    .server_index = 0,
            },
            nacos_detail::NacosRpcModule::Config);

    nacos_detail::NacosBiRequestHandler handlers;
    RpcCaseResult result;
    fiber::async::WaitGroup run_done;
    run_done.add();
    nacos_detail::NacosRpcCloseResult close_result;
    fiber::async::spawn(*loop, [&]() -> DetachedTask {
        close_result = co_await rpc.run(handlers);
        run_done.done();
    });
    stage->store(1, std::memory_order_release);
    auto ready = co_await rpc.wait_ready();
    stage->store(2, std::memory_order_release);
    result.started = ready.has_value();
    result.connection_id = rpc.connection_id();
    rpc.shutdown();
    co_await run_done.join();
    stage->store(3, std::memory_order_release);
    result.stopped = rpc.state() == nacos_detail::NacosRpcState::Stopped;
    result.close_kind = close_result.kind;
    finished->set_value(std::move(result));
}

TEST(NacosRpcTest, RnacosInteropWhenEnabled) {
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
    params.grpc_port = static_cast<std::uint16_t>(port);
    auto config = fiber::nacos::NacosClientConfig::create(std::move(params));
    ASSERT_TRUE(config.has_value());

    fiber::nacos::NacosRpcOptions options;
    options.connect_timeout = 1s;
    options.request_timeout = 1s;
    options.handshake_timeout = 2s;
    options.compatibility_setup_delay = 50ms;
    options.heartbeat_interval = 100ms;

    fiber::event::EventLoopGroup group(1);
    group.start();
    auto finished = std::make_shared<std::promise<RpcCaseResult>>();
    auto future = finished->get_future();
    auto stage = std::make_shared<std::atomic<int>>(0);
    fiber::async::spawn(group.at(0),
                        [loop = &group.at(0), config = std::move(*config), options, finished, stage]() mutable {
                            return run_rnacos_rpc(loop, std::move(config), options, finished, stage);
                        });
    const auto status = future.wait_for(5s);
    ASSERT_EQ(status, std::future_status::ready) << "stage=" << stage->load(std::memory_order_acquire);
    const RpcCaseResult result = future.get();
    group.stop();
    group.join();
    EXPECT_TRUE(result.started);
    EXPECT_TRUE(result.stopped);
}

} // namespace
