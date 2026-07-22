#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <vector>

#include <async/Sleep.h>
#include <async/Spawn.h>
#include <async/Timeout.h>
#include <event/EventLoopGroup.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <fiber/nacos/NamingService.h>
#include <grpc/GrpcFraming.h>
#include <grpc/ProtoCodec.h>
#include <http/Http2Connection.h>
#include <http/HttpBodySpec.h>
#include <http/HttpExchange.h>
#include <http/HttpHeaders.h>
#include <http/HttpTransport.h>
#include <http/ServerRequestFactory.h>
#include <net/SocketAddress.h>
#include <net/TcpListener.h>

#include "../src/naming/NamingServiceImpl.h"

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

class ScriptedNamingServer {
public:
    ScriptedNamingServer(fiber::event::EventLoop &loop, bool reconnect) :
        loop_(&loop), reconnect_(reconnect), connections_to_serve_(reconnect ? 2 : 1),
        handler_([this](fiber::http::HttpExchange &exchange) { return handle(exchange); }),
        factory_(http_options_, handler_), listener_(loop) {
        push_publisher_ = push_watch_.acquire_publisher();
        FIBER_ASSERT(push_publisher_.has_value());
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
    [[nodiscard]] std::size_t subscribe_count() const noexcept { return subscribe_count_; }
    [[nodiscard]] std::size_t unsubscribe_count() const noexcept { return unsubscribe_count_; }
    [[nodiscard]] std::size_t query_count() const noexcept { return query_count_; }
    [[nodiscard]] std::size_t register_count() const noexcept { return register_count_; }
    [[nodiscard]] std::size_t deregister_count() const noexcept { return deregister_count_; }
    [[nodiscard]] std::size_t notify_ack_count() const noexcept { return notify_ack_count_; }
    [[nodiscard]] std::size_t setup_count() const noexcept { return setup_count_; }
    [[nodiscard]] std::uint16_t last_registered_port() const noexcept { return last_registered_port_; }
    [[nodiscard]] bool naming_label_seen() const noexcept { return naming_label_seen_; }

    DetachedTask serve(std::shared_ptr<std::promise<void>> finished) {
        for (std::size_t i = 0; i < connections_to_serve_; ++i) {
            auto accepted = co_await listener_.accept();
            if (!accepted) {
                break;
            }
            auto transport = fiber::http::TcpTransport::create(*loop_, std::move(*accepted), http_options_.tcp);
            if (!transport) {
                break;
            }
            fiber::http::Http2Connection::Options options;
            options.role = fiber::http::Http2Connection::ConnectionRole::Server;
            fiber::http::Http2Connection connection(options, &factory_, fiber::http::ServerRequestFactory::ops());
            active_connection_ = &connection;
            if (connection.start(std::move(*transport)) == fiber::common::IoErr::None) {
                (void) co_await connection.wait_closed();
            }
            active_connection_ = nullptr;
        }
        finished->set_value();
    }

    void close() noexcept {
        closing_ = true;
        push_publisher_->publish(++push_sequence_);
        listener_.close();
        if (active_connection_) {
            active_connection_->shutdown();
        }
    }

private:
    [[nodiscard]] nacos_detail::NacosPayloadMetadata server_metadata() const noexcept {
        return {
                .client_ip = "127.0.0.1",
                .client_version = "server",
                .namespace_id = "namespace",
                .access_token = "server-token",
        };
    }

    template<typename Response>
    fiber::async::Task<void> send_unary(fiber::http::HttpExchange &exchange, const Response &response) {
        if (!(co_await send_response_header(exchange))) {
            co_return;
        }
        auto payload = nacos_detail::encode_payload(response, server_metadata(), 1024 * 1024);
        if (!payload || !(co_await send_payload(exchange, *payload))) {
            co_return;
        }
        co_await send_trailers(exchange);
    }

    dto::NamingServiceInfo make_service_info(std::string_view service, std::string_view group,
                                             std::int64_t last_ref_time) {
        host_ = dto::NamingInstance{};
        host_.ip.set_present("127.0.0.1");
        host_.port = 8080;
        host_.service_name.set_present(service);
        service_info_ = dto::NamingServiceInfo{};
        service_info_.name.set_present(service);
        service_info_.group_name.set_present(group);
        service_info_.clusters.set_present("DEFAULT");
        service_info_.hosts = fiber::json::JsonArray<dto::NamingInstance>(&host_, 1);
        service_info_.last_ref_time = last_ref_time;
        service_info_.checksum.set_present("sum");
        return service_info_;
    }

    DetachedTask reset_connection(fiber::http::Http2Connection *connection) {
        co_await fiber::async::sleep(20ms);
        connection->shutdown();
    }

    void maybe_schedule_reset() {
        if (!reconnect_ || reset_scheduled_ || subscribe_count_ < 2 || register_count_ == 0 || !active_connection_) {
            return;
        }
        reset_scheduled_ = true;
        fiber::http::Http2Connection *connection = active_connection_;
        fiber::async::spawn(*loop_, [this, connection]() { return reset_connection(connection); });
    }

    fiber::async::Task<void> handle_query(fiber::http::HttpExchange &exchange,
                                          const nacos_detail::NacosPayloadView &view) {
        ++query_count_;
        fiber::mem::BufPool pool;
        dto::req::ServiceQueryRequest request;
        if (!nacos_detail::parse_payload_json(view, pool, request)) {
            co_return;
        }
        dto::resp::QueryServiceResponse response;
        response.service_info.set_present(
                make_service_info(request.service_name.value(), request.group_name.value(), ++last_ref_time_));
        co_await send_unary(exchange, response);
    }

    fiber::async::Task<void> handle_subscribe(fiber::http::HttpExchange &exchange,
                                              const nacos_detail::NacosPayloadView &view) {
        fiber::mem::BufPool pool;
        dto::req::SubscribeServiceRequest request;
        if (!nacos_detail::parse_payload_json(view, pool, request)) {
            co_return;
        }
        if (request.subscribe) {
            ++subscribe_count_;
        } else {
            ++unsubscribe_count_;
        }
        dto::resp::SubscribeServiceResponse response;
        response.service_info.set_present(
                make_service_info(request.service_name.value(), request.group_name.value(), ++last_ref_time_));
        co_await send_unary(exchange, response);
        if (request.subscribe) {
            push_publisher_->publish(++push_sequence_);
            maybe_schedule_reset();
        }
    }

    fiber::async::Task<void> handle_instance(fiber::http::HttpExchange &exchange,
                                             const nacos_detail::NacosPayloadView &view) {
        fiber::mem::BufPool pool;
        dto::req::InstanceRequest request;
        if (!nacos_detail::parse_payload_json(view, pool, request)) {
            co_return;
        }
        const std::string_view type = request.type.value();
        if (type == "registerInstance") {
            ++register_count_;
            last_registered_port_ = static_cast<std::uint16_t>(request.instance.value().port);
            co_await fiber::async::sleep(15ms);
        } else if (type == "deregisterInstance") {
            ++deregister_count_;
        }
        dto::resp::InstanceResponse response;
        response.type = request.type;
        co_await send_unary(exchange, response);
        if (type == "registerInstance") {
            maybe_schedule_reset();
        }
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
        if (view->type == dto::req::ServerCheckRequest::kTypeName) {
            dto::resp::ServerCheckResponse response;
            response.connection_id.set_present("naming-connection");
            response.support_ability_negotiation = true;
            co_await send_unary(exchange, response);
        } else if (view->type == dto::req::HealthCheckRequest::kTypeName) {
            co_await send_unary(exchange, dto::resp::HealthCheckResponse{});
        } else if (view->type == dto::req::ServiceQueryRequest::kTypeName) {
            co_await handle_query(exchange, *view);
        } else if (view->type == dto::req::SubscribeServiceRequest::kTypeName) {
            co_await handle_subscribe(exchange, *view);
        } else if (view->type == dto::req::InstanceRequest::kTypeName) {
            co_await handle_instance(exchange, *view);
        }
    }

    fiber::async::Task<void> handle_bidi(fiber::http::HttpExchange &exchange) {
        ++setup_count_;
        fiber::grpc::GrpcFrameReader reader(1024 * 1024);
        auto setup_payload = co_await read_payload(exchange, reader);
        if (!setup_payload || !(co_await send_response_header(exchange))) {
            co_return;
        }
        auto setup_view = nacos_detail::validate_payload(*setup_payload, 1024 * 1024);
        if (setup_view) {
            fiber::mem::BufPool pool;
            dto::req::ConnectionSetupRequest setup;
            if (nacos_detail::parse_payload_json(*setup_view, pool, setup) && setup.labels.is_present()) {
                const auto *module = setup.labels.value().find("module");
                naming_label_seen_ = module && module->value == "naming";
            }
        }
        auto ack = nacos_detail::encode_payload(dto::req::SetupAckRequest{}, server_metadata(), 1024 * 1024);
        if (!ack || !(co_await send_payload(exchange, *ack))) {
            co_return;
        }

        auto pushes = push_watch_.subscribe();
        auto current = pushes.current();
        std::uint64_t version = current.version;
        std::uint64_t sent_sequence = 0;
        while (!closing_) {
            current = pushes.current();
            if (current.value && *current.value > sent_sequence) {
                sent_sequence = *current.value;
                dto::req::NotifySubscriberRequest notify;
                notify.request_id.set_present("notify-" + std::to_string(sent_sequence));
                notify.namespace_id.set_present("namespace");
                notify.service_name.set_present("service");
                notify.group_name.set_present("group");
                notify.service_info.set_present(make_service_info("service", "group", ++last_ref_time_));
                auto payload = nacos_detail::encode_payload(notify, server_metadata(), 1024 * 1024);
                if (!payload || !(co_await send_payload(exchange, *payload))) {
                    break;
                }
                auto response_payload = co_await read_payload(exchange, reader);
                if (!response_payload) {
                    break;
                }
                fiber::mem::BufPool pool;
                dto::resp::NotifySubscriberResponse response;
                if (nacos_detail::decode_payload(*response_payload, 1024 * 1024, pool, response)) {
                    ++notify_ack_count_;
                }
                continue;
            }
            auto next = co_await fiber::async::timeout_for([&pushes, version]() { return pushes.next(version); }, 50ms);
            if (next) {
                version = next->version;
            } else if (next.error() != fiber::common::IoErr::TimedOut) {
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
    bool reconnect_ = false;
    std::size_t connections_to_serve_ = 1;
    fiber::http::HttpServerOptions http_options_;
    fiber::http::HttpHandler handler_;
    fiber::http::ServerRequestFactory factory_;
    fiber::net::TcpListener listener_;
    fiber::http::Http2Connection *active_connection_ = nullptr;
    std::uint16_t port_ = 0;
    fiber::async::Watch<std::uint64_t> push_watch_{0};
    std::optional<fiber::async::Watch<std::uint64_t>::Publisher> push_publisher_;
    dto::NamingInstance host_;
    dto::NamingServiceInfo service_info_;
    std::uint64_t push_sequence_ = 0;
    std::int64_t last_ref_time_ = 0;
    std::size_t subscribe_count_ = 0;
    std::size_t unsubscribe_count_ = 0;
    std::size_t query_count_ = 0;
    std::size_t register_count_ = 0;
    std::size_t deregister_count_ = 0;
    std::size_t notify_ack_count_ = 0;
    std::size_t setup_count_ = 0;
    std::uint16_t last_registered_port_ = 0;
    bool naming_label_seen_ = false;
    bool closing_ = false;
    bool reset_scheduled_ = false;
};

fiber::async::Task<bool> wait_ready(nacos_detail::NamingServiceImpl &service) {
    auto ready = service.subscribe_connection_ready();
    auto current = ready.current();
    std::uint64_t version = current.version;
    for (;;) {
        if (current.value && *current.value) {
            co_return true;
        }
        auto next = co_await fiber::async::timeout_for([&ready, version]() { return ready.next(version); }, 2s);
        if (!next) {
            co_return false;
        }
        current = std::move(*next);
        version = current.version;
    }
}

template<typename Predicate>
fiber::async::Task<bool> wait_until(Predicate predicate) {
    const auto deadline = fiber::event::EventLoop::current().now() + 2s;
    while (!predicate()) {
        if (fiber::event::EventLoop::current().now() >= deadline) {
            co_return false;
        }
        co_await fiber::async::sleep(2ms);
    }
    co_return true;
}

struct NamingCaseResult {
    bool rejects_invalid_arguments = false;
    bool ready = false;
    bool naming_label = false;
    bool pushed = false;
    bool cached_get = false;
    bool queried = false;
    bool shared_subscription = false;
    bool registered = false;
    bool latest_update = false;
    bool deregistered = false;
    bool unsubscribed_after_last = false;
    bool reconnected = false;
    bool stopped = false;
    std::size_t notify_acks = 0;
    std::size_t subscribes = 0;
    std::size_t registrations = 0;
    std::size_t setups = 0;
};

DetachedTask run_case(fiber::event::EventLoop *loop, ScriptedNamingServer *server,
                      fiber::nacos::NacosClientConfig config, fiber::nacos::NamingServiceOptions options,
                      bool reconnect, std::shared_ptr<std::promise<NamingCaseResult>> finished) {
    NamingCaseResult result;
    fiber::async::Watch<fiber::nacos::NacosAuthAccess> auth_watch;
    auto auth_publisher = auth_watch.acquire_publisher();
    FIBER_ASSERT(auth_publisher.has_value());
    auto auth = auth_watch.subscribe();
    auto shared_config = std::make_shared<const fiber::nacos::NacosClientConfig>(std::move(config));
    nacos_detail::NamingServiceImpl service({.loop = loop, .config = std::move(shared_config), .auth = std::move(auth)},
                                            std::move(options));

    auto invalid_get = co_await service.get("", "group");
    auto invalid_subscription = service.subscribe("service", "");
    auto invalid_registration = service.registry("service", "group", fiber::nacos::Instance{});
    result.rejects_invalid_arguments =
            !invalid_get && invalid_get.error().code == fiber::nacos::NamingServiceErrorCode::InvalidArgument &&
            !invalid_subscription &&
            invalid_subscription.error().code == fiber::nacos::NamingServiceErrorCode::InvalidArgument &&
            !invalid_registration &&
            invalid_registration.error().code == fiber::nacos::NamingServiceErrorCode::InvalidArgument;

    auto first = service.subscribe("service", "group");
    auto second = service.subscribe("service", "group");
    auto stopped_subscription = service.subscribe("stopped", "group");
    fiber::nacos::Instance instance{.ip = "127.0.0.1", .port = 8080};
    auto registered = service.registry("service", "group", instance);
    auto status = registered->subscribe_status();
    std::uint64_t status_version = status.current().version;

    FIBER_ASSERT(service.start().has_value());
    auth_publisher->publish({.kind = fiber::nacos::NacosAuthAccessKind::Present, .access_token = "token"});
    result.ready = co_await wait_ready(service);
    result.naming_label = server->naming_label_seen();

    if (result.ready && first && second && registered) {
        auto &subscriber = first->subscriber();
        auto current = subscriber.current();
        auto pushed = co_await fiber::async::timeout_for(
                [&subscriber, version = current.version]() { return subscriber.next(version); }, 2s);
        result.pushed = pushed && pushed->value && pushed->value->kind == fiber::nacos::ResultKind::Success &&
                        pushed->value->data && pushed->value->data->name == "service";

        const std::size_t queries_before = server->query_count();
        auto cached = co_await service.get("service", "group");
        result.cached_get = cached && (*cached)->name == "service" && server->query_count() == queries_before;
        auto queried = co_await service.get("other", "group");
        result.queried = queried && (*queried)->name == "other" && server->query_count() == queries_before + 1;
        // One wire subscription for the two "service" consumers, plus the
        // independent "stopped" subscription used to verify shutdown.
        result.shared_subscription = server->subscribe_count() == 2;

        auto registered_status = co_await fiber::async::timeout_for(
                [&status, status_version]() { return status.next(status_version); }, 2s);
        if (registered_status) {
            status_version = registered_status->version;
        }
        result.registered = registered_status && registered_status->value &&
                            registered_status->value->state == fiber::nacos::RegistrationState::Registered;

        if (reconnect) {
            result.reconnected = co_await wait_until([server]() {
                return server->setup_count() >= 2 && server->subscribe_count() >= 4 && server->register_count() >= 2;
            });
        } else {
            instance.port = 8081;
            (void) registered->update(instance);
            instance.port = 8082;
            (void) registered->update(instance);
            result.latest_update = co_await wait_until(
                    [server]() { return server->last_registered_port() == 8082 && server->register_count() >= 3; });
        }

        first->close();
        co_await fiber::async::sleep(10ms);
        const bool not_yet_unsubscribed = server->unsubscribe_count() == 0;
        second->close();
        result.unsubscribed_after_last =
                not_yet_unsubscribed && co_await wait_until([server]() { return server->unsubscribe_count() == 1; });
        registered->close();
        result.deregistered = co_await wait_until([server]() { return server->deregister_count() == 1; });
    }

    co_await service.shutdown();
    auth_publisher->publish({.kind = fiber::nacos::NacosAuthAccessKind::Stopped});
    if (stopped_subscription) {
        const auto stopped = stopped_subscription->subscriber().current();
        result.stopped = stopped.value && stopped.value->kind == fiber::nacos::ResultKind::Closed;
        stopped_subscription->close();
    }
    result.notify_acks = server->notify_ack_count();
    result.subscribes = server->subscribe_count();
    result.registrations = server->register_count();
    result.setups = server->setup_count();
    finished->set_value(result);
}

NamingCaseResult execute_case(bool reconnect) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto server = std::make_unique<ScriptedNamingServer>(group.at(0), reconnect);
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
    auto config = fiber::nacos::NacosClientConfig::create(std::move(params));
    EXPECT_TRUE(config.has_value());

    fiber::nacos::NamingServiceOptions options;
    options.rpc.connect_timeout = 500ms;
    options.rpc.request_timeout = 500ms;
    options.rpc.handshake_timeout = 1s;
    options.rpc.compatibility_setup_delay = 10ms;
    options.rpc.heartbeat_interval = 1s;
    options.rpc.reconnect_initial_delay = 10ms;
    options.rpc.reconnect_max_delay = 50ms;
    options.rpc.max_inbound_message_bytes = 1024 * 1024;

    auto finished = std::make_shared<std::promise<NamingCaseResult>>();
    auto future = finished->get_future();
    fiber::async::spawn(group.at(0), [loop = &group.at(0), server_ptr = server.get(), config = std::move(*config),
                                      options, reconnect, finished]() mutable {
        return run_case(loop, server_ptr, std::move(config), options, reconnect, finished);
    });

    NamingCaseResult result;
    if (future.wait_for(10s) == std::future_status::ready) {
        result = future.get();
    } else {
        ADD_FAILURE() << "timed out waiting for Nacos NamingService test";
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

TEST(NacosNamingServiceTest, QuerySubscriptionRegistrationAndShutdown) {
    const NamingCaseResult result = execute_case(false);
    EXPECT_TRUE(result.rejects_invalid_arguments);
    EXPECT_TRUE(result.ready);
    EXPECT_TRUE(result.naming_label);
    EXPECT_TRUE(result.pushed);
    EXPECT_TRUE(result.cached_get);
    EXPECT_TRUE(result.queried);
    EXPECT_TRUE(result.shared_subscription);
    EXPECT_TRUE(result.registered);
    EXPECT_TRUE(result.latest_update);
    EXPECT_TRUE(result.deregistered);
    EXPECT_TRUE(result.unsubscribed_after_last);
    EXPECT_TRUE(result.stopped);
    EXPECT_GT(result.notify_acks, 0u);
}

TEST(NacosNamingServiceTest, RestoresSubscriptionAndRegistrationAfterReconnect) {
    const NamingCaseResult result = execute_case(true);
    EXPECT_TRUE(result.ready);
    EXPECT_TRUE(result.reconnected) << "subscribes=" << result.subscribes << " registrations=" << result.registrations
                                    << " setups=" << result.setups;
    EXPECT_TRUE(result.stopped);
}

struct RnacosNamingResult {
    bool ready = false;
    bool registered = false;
    bool queried = false;
    bool subscribed = false;
    bool updated = false;
    bool deregistered = false;
    bool stopped = false;
};

fiber::async::Task<bool> wait_registration_state(fiber::nacos::InstanceRegistration::StatusSubscriber &status,
                                                 std::uint64_t &version, fiber::nacos::RegistrationState expected) {
    for (int i = 0; i < 4; ++i) {
        auto next = co_await fiber::async::timeout_for([&status, version]() { return status.next(version); }, 3s);
        if (!next) {
            co_return false;
        }
        version = next->version;
        if (next->value && next->value->state == expected) {
            co_return true;
        }
    }
    co_return false;
}

fiber::async::Task<bool>
wait_instance_port(fiber::nacos::Subscription<fiber::nacos::ServiceInfo>::Subscriber &subscriber,
                   std::uint64_t &version, std::uint16_t port) {
    for (int i = 0; i < 4; ++i) {
        auto next =
                co_await fiber::async::timeout_for([&subscriber, version]() { return subscriber.next(version); }, 3s);
        if (!next) {
            co_return false;
        }
        version = next->version;
        if (!next->value || next->value->kind != fiber::nacos::ResultKind::Success || !next->value->data) {
            continue;
        }
        for (const fiber::nacos::Instance &host: next->value->data->hosts) {
            if (host.ip == "127.0.0.1" && host.port == port) {
                co_return true;
            }
        }
    }
    co_return false;
}

DetachedTask run_rnacos_naming_case(fiber::event::EventLoop *loop, fiber::nacos::NacosClientConfig config,
                                    fiber::nacos::NamingServiceOptions options,
                                    std::shared_ptr<std::promise<RnacosNamingResult>> finished) {
    constexpr std::string_view kService = "fiber-naming-service-integration";
    constexpr std::string_view kGroup = "DEFAULT_GROUP";
    RnacosNamingResult result;
    fiber::async::Watch<fiber::nacos::NacosAuthAccess> auth_watch;
    auto auth_publisher = auth_watch.acquire_publisher();
    FIBER_ASSERT(auth_publisher.has_value());
    auto auth = auth_watch.subscribe();
    auto shared_config = std::make_shared<const fiber::nacos::NacosClientConfig>(std::move(config));
    nacos_detail::NamingServiceImpl service({.loop = loop, .config = std::move(shared_config), .auth = std::move(auth)},
                                            std::move(options));
    auto subscription_result = service.subscribe(kService, kGroup);
    fiber::nacos::Instance instance{.ip = "127.0.0.1", .port = 19081};
    auto registration_result = service.registry(kService, kGroup, instance);

    FIBER_ASSERT(service.start().has_value());
    auth_publisher->publish(fiber::nacos::NacosAuthAccess{
            .kind = fiber::nacos::NacosAuthAccessKind::Present,
            .access_token = "rnacos-integration-token",
    });
    result.ready = co_await wait_ready(service);

    if (result.ready && subscription_result && registration_result) {
        auto subscription = std::move(*subscription_result);
        auto registration = std::move(*registration_result);
        auto status = registration.subscribe_status();
        std::uint64_t status_version = status.current().version;
        result.registered =
                co_await wait_registration_state(status, status_version, fiber::nacos::RegistrationState::Registered);

        if (result.registered) {
            auto queried = co_await service.get(std::string(kService), std::string(kGroup));
            if (queried) {
                result.queried = std::any_of((*queried)->hosts.begin(), (*queried)->hosts.end(), [](const auto &host) {
                    return host.ip == "127.0.0.1" && host.port == 19081;
                });
            }

            auto &subscriber = subscription.subscriber();
            std::uint64_t service_version = subscriber.current().version;
            result.subscribed = co_await wait_instance_port(subscriber, service_version, 19081);

            instance.port = 19082;
            const auto update = registration.update(instance);
            const bool update_registered = co_await wait_registration_state(
                    status, status_version, fiber::nacos::RegistrationState::Registered);
            const bool update_pushed = co_await wait_instance_port(subscriber, service_version, 19082);
            result.updated = update.has_value() && update_registered && update_pushed;
        }

        registration.close();
        result.deregistered =
                co_await wait_registration_state(status, status_version, fiber::nacos::RegistrationState::Closed);
        subscription.close();
    }

    co_await service.shutdown();
    auth_publisher->publish(fiber::nacos::NacosAuthAccess{.kind = fiber::nacos::NacosAuthAccessKind::Stopped});
    result.stopped = true;
    finished->set_value(result);
}

TEST(NacosNamingServiceTest, RnacosInteropWhenEnabled) {
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

    fiber::nacos::NamingServiceOptions options;
    options.rpc.connect_timeout = 1s;
    options.rpc.request_timeout = 2s;
    options.rpc.handshake_timeout = 3s;
    options.rpc.compatibility_setup_delay = 50ms;
    options.rpc.heartbeat_interval = 1s;
    options.rpc.reconnect_initial_delay = 20ms;
    options.rpc.reconnect_max_delay = 100ms;

    fiber::event::EventLoopGroup group(1);
    group.start();
    auto finished = std::make_shared<std::promise<RnacosNamingResult>>();
    auto future = finished->get_future();
    fiber::async::spawn(group.at(0), [loop = &group.at(0), config = std::move(*config), options, finished]() mutable {
        return run_rnacos_naming_case(loop, std::move(config), options, finished);
    });
    ASSERT_EQ(future.wait_for(20s), std::future_status::ready);
    const RnacosNamingResult result = future.get();
    group.stop();
    group.join();

    EXPECT_TRUE(result.ready);
    EXPECT_TRUE(result.registered);
    EXPECT_TRUE(result.queried);
    EXPECT_TRUE(result.subscribed);
    EXPECT_TRUE(result.updated);
    EXPECT_TRUE(result.deregistered);
    EXPECT_TRUE(result.stopped);
}

} // namespace
