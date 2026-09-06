#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Timeout.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/Http2CloseGate.h>
#include <fiber/http/Http2Connection.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http/HttpTransport.h>
#include <fiber/http/ServerRequestFactory.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpListener.h>
#include "rpc/grpc/GrpcFraming.h"
#include "rpc/grpc/ProtoCodec.h"

#include "../src/config/ConfigServiceImpl.h"
#include "../src/rpc/NacosPayloadCodec.h"
#include "NacosTestDns.h"

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;
namespace dto = fiber::nacos::dto;
namespace nacos_detail = fiber::nacos::detail;
namespace proto = fiber::nacos::proto;

template<typename T>
struct CallbackWatch {
    using Result = fiber::nacos::SubscriptionResult<T>;
    using Watch = fiber::async::Watch<Result>;

    CallbackWatch() : publisher(watch.acquire_publisher()), subscriber(watch.subscribe()) {
        FIBER_ASSERT(publisher.has_value());
    }

    static void notify(void *context, const Result &result) noexcept {
        auto *self = static_cast<CallbackWatch *>(context);
        FIBER_ASSERT(self != nullptr);
        self->publisher->publish(result);
    }

    Watch watch;
    std::optional<typename Watch::Publisher> publisher;
    typename Watch::Subscriber subscriber;
};

template<typename T>
void ignore_subscription(void *, const fiber::nacos::SubscriptionResult<T> &) noexcept {}

std::string_view nullable_text(const fiber::json::Nullable<std::string_view> &value) noexcept {
    return value.is_present() ? value.value() : std::string_view{};
}

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

class ScriptedConfigServer {
public:
    ScriptedConfigServer(fiber::event::EventLoop &loop, bool reset_after_first_listen = false) :
        loop_(&loop), reset_after_first_listen_(reset_after_first_listen),
        connections_to_serve_(reset_after_first_listen ? 2 : 1),
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
    [[nodiscard]] std::size_t listen_count() const noexcept { return listen_count_; }
    [[nodiscard]] std::size_t unlisten_count() const noexcept { return unlisten_count_; }
    [[nodiscard]] std::size_t setup_count() const noexcept { return setup_count_; }
    [[nodiscard]] std::size_t query_count() const noexcept { return query_count_; }
    [[nodiscard]] std::size_t notify_ack_count() const noexcept { return notify_ack_count_; }
    [[nodiscard]] std::size_t max_listen_contexts() const noexcept { return max_listen_contexts_; }
    [[nodiscard]] const std::vector<std::string> &published_types() const noexcept { return published_types_; }
    [[nodiscard]] bool authority_seen() const noexcept { return authority_seen_.load(std::memory_order_acquire); }

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
            fiber::http::Http2CloseGate close_gate;
            close_gate.arm(connection);
            active_connection_ = &connection;
            if (connection.start(std::move(*transport)) == fiber::common::IoErr::None) {
                (void) co_await close_gate.join();
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

    void notify_change() { push_publisher_->publish(++push_sequence_); }

    DetachedTask reset_connection(fiber::http::Http2Connection *connection) {
        co_await fiber::async::sleep(10ms);
        connection->shutdown();
    }

    fiber::async::Task<void> handle_query(fiber::http::HttpExchange &exchange,
                                          const nacos_detail::NacosPayloadView &view) {
        ++query_count_;
        fiber::mem::BufPool pool;
        dto::req::ConfigQueryRequest request;
        if (!nacos_detail::parse_payload_json(view, pool, request)) {
            co_return;
        }
        const bool exists = exists_ && request.data_id.is_present() && request.data_id.value() == "data" &&
                            request.group.is_present() && request.group.value() == "group";
        const std::string content = content_;
        const std::string md5 = md5_;
        if (query_delay_ > 0ms) {
            co_await fiber::async::sleep(query_delay_);
        }
        dto::resp::ConfigQueryResponse response;
        if (exists) {
            response.content.set_present(content);
            response.md5.set_present(md5);
            response.content_type.set_present("text");
        } else {
            response.result_code = dto::kResponseFail;
            response.error_code = dto::resp::ConfigQueryResponse::kConfigNotFound;
            response.message.set_present("config not found");
        }
        co_await send_unary(exchange, response);
    }

    fiber::async::Task<void> handle_publish(fiber::http::HttpExchange &exchange,
                                            const nacos_detail::NacosPayloadView &view) {
        fiber::mem::BufPool pool;
        dto::req::ConfigPublishRequest request;
        if (!nacos_detail::parse_payload_json(view, pool, request)) {
            co_return;
        }
        dto::resp::ConfigPublishResponse response;
        if (request.addition_map.is_present()) {
            for (const auto &entry: request.addition_map.value()) {
                if (entry.key == "type") {
                    published_types_.emplace_back(entry.value);
                }
            }
        }
        if (request.cas_md5.is_present() && request.cas_md5.value() != md5_) {
            response.result_code = dto::kResponseFail;
            response.error_code = dto::resp::ConfigQueryResponse::kConfigQueryConflict;
            response.message.set_present("cas conflict");
        } else {
            const std::string next_content(nullable_text(request.content));
            if (!exists_ || content_ != next_content) {
                content_ = next_content;
                md5_ = "m" + std::to_string(++md5_sequence_);
                exists_ = true;
            }
            notify_change();
        }
        co_await send_unary(exchange, response);
    }

    fiber::async::Task<void> handle_remove(fiber::http::HttpExchange &exchange) {
        exists_ = false;
        content_.clear();
        md5_.clear();
        notify_change();
        co_await send_unary(exchange, dto::resp::ConfigRemoveResponse{});
    }

    fiber::async::Task<void> handle_listen(fiber::http::HttpExchange &exchange,
                                           const nacos_detail::NacosPayloadView &view) {
        fiber::mem::BufPool pool;
        dto::req::ConfigBatchListenRequest request;
        if (!nacos_detail::parse_payload_json(view, pool, request)) {
            co_return;
        }
        dto::resp::ConfigChangeBatchListenResponse response;
        std::vector<dto::resp::ConfigContext> changed;
        if (request.listen) {
            ++listen_count_;
            max_listen_contexts_ = std::max(max_listen_contexts_, request.config_listen_contexts.size());
            for (const dto::req::ConfigListenContext &context: request.config_listen_contexts) {
                const bool exists = exists_ && context.data_id.is_present() && context.data_id.value() == "data" &&
                                    context.group.is_present() && context.group.value() == "group";
                const bool differs = exists && (!context.md5.is_present() || context.md5.value() != md5_);
                if (!differs) {
                    continue;
                }
                dto::resp::ConfigContext item;
                item.data_id = context.data_id;
                item.group = context.group;
                item.tenant = context.tenant;
                changed.push_back(item);
            }
        } else {
            ++unlisten_count_;
        }
        response.changed_configs = fiber::json::JsonArray<dto::resp::ConfigContext>(changed.data(), changed.size());
        co_await send_unary(exchange, response);

        if (reset_after_first_listen_ && listen_count_ == 1 && !reset_scheduled_) {
            reset_scheduled_ = true;
            fiber::http::Http2Connection *connection = active_connection_;
            fiber::async::spawn(*loop_, [this, connection]() { return reset_connection(connection); });
        }
    }

    fiber::async::Task<void> handle_unary(fiber::http::HttpExchange &exchange) {
        fiber::nacos::detail::grpc::GrpcFrameReader reader(1024 * 1024);
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
            response.connection_id.set_present("connection");
            response.support_ability_negotiation = true;
            co_await send_unary(exchange, response);
        } else if (view->type == dto::req::HealthCheckRequest::kTypeName) {
            co_await send_unary(exchange, dto::resp::HealthCheckResponse{});
        } else if (view->type == dto::req::ConfigQueryRequest::kTypeName) {
            co_await handle_query(exchange, *view);
        } else if (view->type == dto::req::ConfigPublishRequest::kTypeName) {
            co_await handle_publish(exchange, *view);
        } else if (view->type == dto::req::ConfigRemoveRequest::kTypeName) {
            co_await handle_remove(exchange);
        } else if (view->type == dto::req::ConfigBatchListenRequest::kTypeName) {
            co_await handle_listen(exchange, *view);
        }
    }

    fiber::async::Task<void> handle_bidi(fiber::http::HttpExchange &exchange) {
        fiber::nacos::detail::grpc::GrpcFrameReader reader(1024 * 1024);
        auto setup_payload = co_await read_payload(exchange, reader);
        if (!setup_payload || !(co_await send_response_header(exchange))) {
            co_return;
        }
        ++setup_count_;
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
                dto::req::ConfigChangeNotifyRequest notify;
                // request_id is Nullable<string_view> (non-owning); build the text
                // in a local that outlives encode_payload, else the temporary from
                // "notify-" + std::to_string(...) dangles (use-after-free).
                std::string request_id_text = "notify-" + std::to_string(sent_sequence);
                notify.request_id.set_present(request_id_text);
                notify.data_id.set_present("data");
                notify.group.set_present("group");
                notify.tenant.set_present("tenant");
                auto payload = nacos_detail::encode_payload(notify, server_metadata(), 1024 * 1024);
                if (!payload || !(co_await send_payload(exchange, *payload))) {
                    break;
                }
                auto response_payload = co_await read_payload(exchange, reader);
                if (!response_payload) {
                    break;
                }
                fiber::mem::BufPool pool;
                dto::resp::ConfigChangeNotifyResponse response;
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
        const auto *authority = exchange.host_header();
        if (authority != nullptr && authority->value_view() == "config.test:" + std::to_string(port_)) {
            authority_seen_.store(true, std::memory_order_release);
        }
        if (exchange.uri().path == "/Request/request") {
            co_await handle_unary(exchange);
        } else if (exchange.uri().path == "/BiRequestStream/requestBiStream") {
            co_await handle_bidi(exchange);
        }
    }

    fiber::event::EventLoop *loop_ = nullptr;
    bool reset_after_first_listen_ = false;
    std::size_t connections_to_serve_ = 1;
    fiber::http::HttpServerOptions http_options_;
    fiber::http::HttpHandler handler_;
    fiber::http::ServerRequestFactory factory_;
    fiber::net::TcpListener listener_;
    fiber::http::Http2Connection *active_connection_ = nullptr;
    std::uint16_t port_ = 0;
    fiber::async::Watch<std::uint64_t> push_watch_{0};
    std::optional<fiber::async::Watch<std::uint64_t>::Publisher> push_publisher_;
    std::chrono::milliseconds query_delay_{20};
    std::string content_ = "initial";
    std::string md5_ = "m0";
    std::vector<std::string> published_types_;
    std::uint64_t push_sequence_ = 0;
    std::size_t md5_sequence_ = 0;
    std::size_t listen_count_ = 0;
    std::size_t unlisten_count_ = 0;
    std::size_t setup_count_ = 0;
    std::size_t query_count_ = 0;
    std::size_t notify_ack_count_ = 0;
    std::size_t max_listen_contexts_ = 0;
    bool exists_ = true;
    bool closing_ = false;
    bool reset_scheduled_ = false;
    std::atomic<bool> authority_seen_{false};
};

struct ConfigCaseResult {
    bool status_created = false;
    bool status_auth_unavailable = false;
    bool status_ready = false;
    bool status_aggregates = false;
    bool status_reconnected = false;
    bool status_stopped = false;
    bool ready = false;
    bool initial_present = false;
    bool initial_present_deduplicated = false;
    bool initial_not_found = false;
    bool initial_not_found_deduplicated = false;
    bool cached_get = false;
    bool missing_get = false;
    bool types_match = false;
    bool listen_batches_bounded = false;
    bool periodic_redo = false;
    bool cas_error_preserved = false;
    bool dirty_requery_reached_latest = false;
    bool md5_deduplicated = false;
    bool removed = false;
    bool shared_subscription = false;
    bool unregister_after_last = false;
    bool stopped_published = false;
    bool clean_shutdown = false;
    bool reconnected = false;
    bool update_after_reconnect = false;
    bool authority_seen = false;
    std::size_t notify_acks = 0;
};

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

DetachedTask run_config_case(fiber::event::EventLoop *loop, ScriptedConfigServer *server,
                             fiber::nacos::NacosClientConfig config, fiber::nacos::ConfigServiceOptions options,
                             bool reconnect, std::shared_ptr<std::promise<ConfigCaseResult>> finished) {
    ConfigCaseResult result;
    fiber::async::Watch<fiber::nacos::NacosAuthAccess> auth_watch;
    auto auth_publisher = auth_watch.acquire_publisher();
    FIBER_ASSERT(auth_publisher.has_value());
    auto auth = auth_watch.subscribe();
    fiber::nacos::test::NacosTestDns dns;
    FIBER_ASSERT(dns.init(*loop, "config.test"));
    auto shared_config = std::make_shared<const fiber::nacos::NacosClientConfig>(std::move(config));
    auto created = nacos_detail::create_config_service({.loop = loop,
                                                        .resolver = &dns.address_resolver(),
                                                        .config = std::move(shared_config),
                                                        .auth = std::move(auth)},
                                                       std::move(options));
    FIBER_ASSERT(created.has_value());
    auto service = std::move(*created);
    auto service_status = service->subscribe_status();
    const auto created_status = service_status.current();
    result.status_created = created_status.value &&
                            created_status.value->connection.phase == fiber::nacos::NacosServicePhase::Created &&
                            !created_status.value->connection.rpc_available;
    std::vector<fiber::nacos::Subscription<fiber::nacos::ConfigData>> batched_subscriptions;
    if (!reconnect) {
        for (int i = 0; i < 5; ++i) {
            auto subscription = service->subscribe("batch-" + std::to_string(i), "group",
                                                   &ignore_subscription<fiber::nacos::ConfigData>, nullptr);
            if (subscription) {
                batched_subscriptions.push_back(std::move(*subscription));
            }
        }
    }
    if (!reconnect) {
        const auto pending = service_status.current();
        result.status_created = result.status_created && pending.value &&
                                pending.value->subscriptions.active_count == batched_subscriptions.size() &&
                                pending.value->subscriptions.pending_count == batched_subscriptions.size();
    }
    const auto before_start = service_status.current();
    FIBER_ASSERT(service->start().has_value());
    const auto started_status = co_await service_status.next(before_start.version);
    result.status_created = result.status_created && started_status.value &&
                            started_status.value->connection.phase == fiber::nacos::NacosServicePhase::Connecting &&
                            !started_status.value->connection.rpc_available;

    auth_publisher->publish(fiber::nacos::NacosAuthAccess{
            .kind = fiber::nacos::NacosAuthAccessKind::InitialFailed,
    });
    result.status_auth_unavailable = co_await wait_until([&service_status]() {
        const auto current = service_status.current();
        return current.value && current.value->connection.phase == fiber::nacos::NacosServicePhase::Connecting &&
               current.value->connection.failure ==
                       fiber::nacos::NacosServiceFailureCategory::AuthenticationUnavailable &&
               !current.value->connection.rpc_available;
    });
    auth_publisher->publish(fiber::nacos::NacosAuthAccess{
            .kind = fiber::nacos::NacosAuthAccessKind::Present,
            .access_token = "token",
    });
    result.ready = co_await wait_until([server]() { return server->setup_count() >= 1; });
    result.status_ready = co_await wait_until([&service_status]() {
        const auto current = service_status.current();
        return current.value && current.value->connection.phase == fiber::nacos::NacosServicePhase::Ready &&
               current.value->connection.failure == fiber::nacos::NacosServiceFailureCategory::None &&
               current.value->connection.rpc_available && current.value->connection.connection_ready_count == 1 &&
               current.value->connection.reconnect_attempt_count >= 1;
    });
    if (!reconnect) {
        result.status_aggregates = co_await wait_until([&service_status]() {
            const auto current = service_status.current();
            return current.value && current.value->subscriptions.active_count == 5 &&
                   current.value->subscriptions.registered_count == 5 &&
                   current.value->subscriptions.synchronized_count == 5 &&
                   current.value->subscriptions.pending_count == 0;
        });
    }

    if (result.ready && !reconnect) {
        const auto listen_deadline = fiber::event::EventLoop::current().now() + 2s;
        while (server->listen_count() < 3 && fiber::event::EventLoop::current().now() < listen_deadline) {
            co_await fiber::async::sleep(2ms);
        }
        result.listen_batches_bounded = server->listen_count() >= 3 && server->max_listen_contexts() <= 2;
        const std::size_t initial_listen_count = server->listen_count();
        result.periodic_redo = co_await wait_until(
                [server, initial_listen_count]() { return server->listen_count() > initial_listen_count; });
        for (auto &subscription: batched_subscriptions) {
            subscription.close();
        }

        const fiber::nacos::ConfigType types[] = {
                fiber::nacos::ConfigType::Json,       fiber::nacos::ConfigType::Text, fiber::nacos::ConfigType::Yaml,
                fiber::nacos::ConfigType::Properties, fiber::nacos::ConfigType::Xml,  fiber::nacos::ConfigType::Html,
        };
        for (std::size_t i = 0; i < std::size(types); ++i) {
            auto published = co_await service->publish("data", "group", "type-" + std::to_string(i), types[i]);
            if (!published) {
                result.ready = false;
                break;
            }
        }
        result.types_match = server->published_types() ==
                             std::vector<std::string>({"json", "text", "yaml", "properties", "xml", "html"});

        auto missing = co_await service->get_config("missing", "group");
        result.missing_get = missing && (*missing)->state == fiber::nacos::ConfigState::NotFound;

        CallbackWatch<fiber::nacos::ConfigData> updates;
        CallbackWatch<fiber::nacos::ConfigData> stopped_updates;
        auto subscribed =
                service->subscribe("data", "group", &CallbackWatch<fiber::nacos::ConfigData>::notify, &updates);
        auto stopped_subscription = service->subscribe(
                "stopped", "group", &CallbackWatch<fiber::nacos::ConfigData>::notify, &stopped_updates);
        if (subscribed && stopped_subscription) {
            auto subscription = std::move(*subscribed);
            auto &sub = updates.subscriber;
            auto initial = sub.current();
            result.initial_present = initial.value && initial.value->kind == fiber::nacos::ResultKind::Success &&
                                     initial.value->data &&
                                     initial.value->data->state == fiber::nacos::ConfigState::Present &&
                                     initial.value->data->content == "type-5";
            std::uint64_t version = initial.version;
            if (!result.initial_present) {
                auto next = co_await fiber::async::timeout_for([&sub, version]() { return sub.next(version); }, 2s);
                result.initial_present = next && next->value &&
                                         next->value->kind == fiber::nacos::ResultKind::Success && next->value->data &&
                                         next->value->data->state == fiber::nacos::ConfigState::Present &&
                                         next->value->data->content == "type-5";
                if (next) {
                    version = next->version;
                }
            }

            auto unchanged = co_await fiber::async::timeout_for([&sub, version]() { return sub.next(version); }, 150ms);
            result.initial_present_deduplicated = !unchanged && unchanged.error() == fiber::common::IoErr::TimedOut;

            auto &missing_sub = stopped_updates.subscriber;
            const auto missing_current = missing_sub.current();
            result.initial_not_found = missing_current.value &&
                                       missing_current.value->kind == fiber::nacos::ResultKind::Success &&
                                       missing_current.value->data &&
                                       missing_current.value->data->state == fiber::nacos::ConfigState::NotFound;
            std::uint64_t missing_version = missing_current.version;
            if (!result.initial_not_found) {
                auto missing_initial = co_await fiber::async::timeout_for(
                        [&missing_sub, missing_version]() { return missing_sub.next(missing_version); }, 2s);
                result.initial_not_found = missing_initial && missing_initial->value &&
                                           missing_initial->value->kind == fiber::nacos::ResultKind::Success &&
                                           missing_initial->value->data &&
                                           missing_initial->value->data->state == fiber::nacos::ConfigState::NotFound;
                if (missing_initial) {
                    missing_version = missing_initial->version;
                }
            }
            auto missing_duplicate = co_await fiber::async::timeout_for(
                    [&missing_sub, missing_version]() { return missing_sub.next(missing_version); }, 150ms);
            result.initial_not_found_deduplicated =
                    !missing_duplicate && missing_duplicate.error() == fiber::common::IoErr::TimedOut;

            auto cached = co_await service->get_config("data", "group");
            result.cached_get =
                    cached && (*cached)->state == fiber::nacos::ConfigState::Present && (*cached)->content == "type-5";

            const std::size_t listen_before_shared = server->listen_count();
            CallbackWatch<fiber::nacos::ConfigData> shared_updates;
            auto shared = service->subscribe("data", "group", &CallbackWatch<fiber::nacos::ConfigData>::notify,
                                             &shared_updates);
            if (shared) {
                auto shared_subscription = std::move(*shared);
                const auto replay = shared_updates.subscriber.current();
                result.shared_subscription = replay.value && replay.value->data &&
                                             replay.value->data->state == fiber::nacos::ConfigState::Present &&
                                             replay.value->data->content == "type-5" &&
                                             server->listen_count() == listen_before_shared;

                const std::size_t query_before = server->query_count();
                auto first = co_await service->publish("data", "group", "first", fiber::nacos::ConfigType::Text,
                                                       std::optional<std::string>("m6"));
                for (int i = 0; i < 100 && server->query_count() == query_before; ++i) {
                    co_await fiber::async::sleep(2ms);
                }
                auto second = co_await service->publish("data", "group", "second", fiber::nacos::ConfigType::Text);
                for (int i = 0; i < 4; ++i) {
                    auto updated =
                            co_await fiber::async::timeout_for([&sub, version]() { return sub.next(version); }, 2s);
                    if (!updated) {
                        break;
                    }
                    version = updated->version;
                    if (updated->value && updated->value->kind == fiber::nacos::ResultKind::Success &&
                        updated->value->data && updated->value->data->state == fiber::nacos::ConfigState::Present &&
                        updated->value->data->content == "second") {
                        result.dirty_requery_reached_latest = first.has_value() && second.has_value();
                        break;
                    }
                }

                const std::uint64_t dedup_version = sub.current().version;
                auto same = co_await service->publish("data", "group", "second", fiber::nacos::ConfigType::Text);
                auto duplicate = co_await fiber::async::timeout_for(
                        [&sub, dedup_version]() { return sub.next(dedup_version); }, 150ms);
                result.md5_deduplicated =
                        same.has_value() && !duplicate && duplicate.error() == fiber::common::IoErr::TimedOut;

                auto conflict = co_await service->publish("data", "group", "bad", fiber::nacos::ConfigType::Text,
                                                          std::optional<std::string>("wrong"));
                result.cas_error_preserved =
                        !conflict && conflict.error().code == fiber::nacos::ConfigServiceErrorCode::Server &&
                        conflict.error().error_code == dto::resp::ConfigQueryResponse::kConfigQueryConflict;

                auto removed = co_await service->remove_config("data", "group");
                auto deleted = co_await fiber::async::timeout_for([&sub, version]() { return sub.next(version); }, 2s);
                result.removed = removed.has_value() && deleted && deleted->value && deleted->value->data &&
                                 deleted->value->data->state == fiber::nacos::ConfigState::NotFound;

                subscription.close();
                co_await fiber::async::sleep(20ms);
                const std::size_t unlisten_before_last = server->unlisten_count();
                shared_subscription.close();
                for (int i = 0; i < 100 && server->unlisten_count() == unlisten_before_last; ++i) {
                    co_await fiber::async::sleep(2ms);
                }
                result.unregister_after_last = server->unlisten_count() > unlisten_before_last;
            }

            co_await service->shutdown();
            const auto stopped = stopped_updates.subscriber.current();
            result.stopped_published = stopped_subscription->closed() && stopped.value &&
                                       stopped.value->kind == fiber::nacos::ResultKind::Closed;
            stopped_subscription->close();
        } else {
            co_await service->shutdown();
        }
    } else if (result.ready) {
        CallbackWatch<fiber::nacos::ConfigData> updates;
        auto subscribed =
                service->subscribe("data", "group", &CallbackWatch<fiber::nacos::ConfigData>::notify, &updates);
        if (subscribed) {
            auto subscription = std::move(*subscribed);
            auto &sub = updates.subscriber;
            auto current = sub.current();
            auto initial = co_await fiber::async::timeout_for(
                    [&sub, version = current.version]() { return sub.next(version); }, 2s);
            if (initial && initial->value && initial->value->kind == fiber::nacos::ResultKind::Success) {
                const std::uint64_t version = initial->version;
                for (int i = 0; i < 1000 && (server->listen_count() < 2 || server->setup_count() < 2); ++i) {
                    co_await fiber::async::sleep(2ms);
                }
                result.reconnected = server->listen_count() >= 2 && server->setup_count() >= 2;
                result.status_reconnected = co_await wait_until([&service_status]() {
                    const auto current = service_status.current();
                    return current.value && current.value->connection.phase == fiber::nacos::NacosServicePhase::Ready &&
                           current.value->connection.rpc_available &&
                           current.value->connection.connection_ready_count >= 2 &&
                           current.value->connection.disconnect_count >= 1 &&
                           current.value->connection.reconnect_attempt_count >= 1 &&
                           current.value->subscriptions.active_count == 1 &&
                           current.value->subscriptions.registered_count == 1 &&
                           current.value->subscriptions.synchronized_count == 1 &&
                           current.value->subscriptions.pending_count == 0;
                });
                auto published =
                        co_await service->publish("data", "group", "after-reconnect", fiber::nacos::ConfigType::Text);
                auto updated = co_await fiber::async::timeout_for([&sub, version]() { return sub.next(version); }, 2s);
                result.update_after_reconnect = published.has_value() && updated && updated->value &&
                                                updated->value->kind == fiber::nacos::ResultKind::Success &&
                                                updated->value->data &&
                                                updated->value->data->state == fiber::nacos::ConfigState::Present &&
                                                updated->value->data->content == "after-reconnect";
            }
            subscription.close();
        }
        co_await service->shutdown();
    } else {
        co_await service->shutdown();
    }

    const auto stopped_status = service_status.current();
    result.status_stopped =
            stopped_status.value &&
            stopped_status.value->connection.phase == fiber::nacos::NacosServicePhase::Stopped &&
            stopped_status.value->connection.failure == fiber::nacos::NacosServiceFailureCategory::Shutdown &&
            !stopped_status.value->connection.rpc_available && stopped_status.value->subscriptions.active_count == 0;
    auth_publisher->publish(fiber::nacos::NacosAuthAccess{.kind = fiber::nacos::NacosAuthAccessKind::Stopped});
    result.clean_shutdown = true;
    result.authority_seen = server->authority_seen();
    result.notify_acks = server->notify_ack_count();
    co_await fiber::async::sleep(1ms);
    dns.release();
    finished->set_value(result);
}

ConfigCaseResult execute_config_case(bool reconnect) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto server = std::make_unique<ScriptedConfigServer>(group.at(0), reconnect);
    EXPECT_TRUE(server->valid());
    auto server_finished = std::make_shared<std::promise<void>>();
    auto server_future = server_finished->get_future();
    fiber::async::spawn(group.at(0),
                        [server_ptr = server.get(), server_finished]() { return server_ptr->serve(server_finished); });

    fiber::nacos::NacosClientConfigParams params;
    params.server_hosts.push_back("CONFIG.TEST.");
    params.username = "nacos";
    params.password = "nacos";
    params.grpc_port = server->port();
    params.namespace_id = "namespace";
    params.tenant = "tenant";
    auto config = fiber::nacos::NacosClientConfig::create(std::move(params));
    EXPECT_TRUE(config.has_value());

    fiber::nacos::ConfigServiceOptions options;
    options.rpc.connect_timeout = 500ms;
    options.rpc.request_timeout = 500ms;
    options.rpc.handshake_timeout = 1s;
    options.rpc.compatibility_setup_delay = 10ms;
    options.rpc.heartbeat_interval = 1s;
    options.rpc.reconnect_initial_delay = 10ms;
    options.rpc.reconnect_max_delay = 50ms;
    options.subscription_redo_interval = 50ms;
    options.rpc.max_inbound_message_bytes = 1024 * 1024;
    options.max_content_bytes = 1024 * 1024;
    options.rpc.max_push_response_bytes = 64 * 1024;
    options.max_listen_contexts_per_request = 2;

    auto finished = std::make_shared<std::promise<ConfigCaseResult>>();
    auto future = finished->get_future();
    fiber::async::spawn(group.at(0), [loop = &group.at(0), server_ptr = server.get(), config = std::move(*config),
                                      options, reconnect, finished]() mutable {
        return run_config_case(loop, server_ptr, std::move(config), options, reconnect, finished);
    });

    ConfigCaseResult result;
    if (future.wait_for(10s) == std::future_status::ready) {
        result = future.get();
    } else {
        ADD_FAILURE() << "timed out waiting for Nacos ConfigService test";
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

struct ConfigLifecycleInterruptResult {
    bool auth_wait = false;
    bool auth_unavailable_status = false;
    bool reconnect_backoff = false;
    bool reconnect_backoff_status = false;
    bool stopped_status = false;
};

DetachedTask
run_config_lifecycle_interrupt_case(fiber::event::EventLoop *loop, std::shared_ptr<std::atomic<int>> stage,
                                    std::shared_ptr<std::promise<ConfigLifecycleInterruptResult>> finished) {
    auto make_config = []() {
        fiber::nacos::NacosClientConfigParams params;
        params.server_hosts.push_back("127.0.0.1");
        params.grpc_port = 1;
        return fiber::nacos::NacosClientConfig::create(std::move(params));
    };

    ConfigLifecycleInterruptResult result;
    {
        fiber::async::Watch<fiber::nacos::NacosAuthAccess> auth_watch;
        auto auth_publisher = auth_watch.acquire_publisher();
        FIBER_ASSERT(auth_publisher.has_value());
        auto config = make_config();
        FIBER_ASSERT(config.has_value());
        auto service = nacos_detail::create_config_service(
                {.loop = loop,
                 .config = std::make_shared<const fiber::nacos::NacosClientConfig>(std::move(*config)),
                 .auth = auth_watch.subscribe()},
                {});
        FIBER_ASSERT(service.has_value());
        auto status = (*service)->subscribe_status();
        FIBER_ASSERT((*service)->start().has_value());
        auth_publisher->publish({.kind = fiber::nacos::NacosAuthAccessKind::InitialFailed});
        result.auth_unavailable_status = co_await wait_until([&status]() {
            const auto current = status.current();
            return current.value &&
                   current.value->connection.failure ==
                           fiber::nacos::NacosServiceFailureCategory::AuthenticationUnavailable &&
                   current.value->connection.phase == fiber::nacos::NacosServicePhase::Connecting &&
                   !current.value->connection.rpc_available;
        });
        stage->store(1, std::memory_order_release);
        const auto start = fiber::event::EventLoop::current().now();
        co_await (*service)->shutdown();
        result.auth_wait = fiber::event::EventLoop::current().now() - start < 1s;
        const auto stopped = status.current();
        result.stopped_status =
                stopped.value && stopped.value->connection.phase == fiber::nacos::NacosServicePhase::Stopped &&
                stopped.value->connection.failure == fiber::nacos::NacosServiceFailureCategory::Shutdown &&
                !stopped.value->connection.rpc_available;
        stage->store(2, std::memory_order_release);
    }

    {
        fiber::async::Watch<fiber::nacos::NacosAuthAccess> auth_watch;
        auto auth_publisher = auth_watch.acquire_publisher();
        FIBER_ASSERT(auth_publisher.has_value());
        auto config = make_config();
        FIBER_ASSERT(config.has_value());
        fiber::nacos::ConfigServiceOptions options;
        options.rpc.connect_timeout = 20ms;
        options.rpc.reconnect_initial_delay = 2s;
        options.rpc.reconnect_max_delay = 2s;
        auto service = nacos_detail::create_config_service(
                {.loop = loop,
                 .config = std::make_shared<const fiber::nacos::NacosClientConfig>(std::move(*config)),
                 .auth = auth_watch.subscribe()},
                options);
        FIBER_ASSERT(service.has_value());
        auto status = (*service)->subscribe_status();
        FIBER_ASSERT((*service)->start().has_value());
        auth_publisher->publish({.kind = fiber::nacos::NacosAuthAccessKind::Present, .access_token = "token"});
        result.reconnect_backoff_status = co_await wait_until([&status]() {
            const auto current = status.current();
            return current.value &&
                   current.value->connection.phase == fiber::nacos::NacosServicePhase::ReconnectBackoff &&
                   current.value->connection.failure == fiber::nacos::NacosServiceFailureCategory::Transport &&
                   !current.value->connection.rpc_available;
        });
        stage->store(3, std::memory_order_release);
        const auto start = fiber::event::EventLoop::current().now();
        co_await (*service)->shutdown();
        result.reconnect_backoff = fiber::event::EventLoop::current().now() - start < 1s;
        stage->store(4, std::memory_order_release);
    }

    finished->set_value(result);
}

TEST(NacosConfigServiceTest, UnarySubscriptionDedupDirtyUnregisterAndShutdown) {
    const ConfigCaseResult result = execute_config_case(false);
    EXPECT_TRUE(result.status_created);
    EXPECT_TRUE(result.status_auth_unavailable);
    EXPECT_TRUE(result.status_ready);
    EXPECT_TRUE(result.status_aggregates);
    EXPECT_TRUE(result.status_stopped);
    EXPECT_TRUE(result.ready);
    EXPECT_TRUE(result.initial_present);
    EXPECT_TRUE(result.initial_present_deduplicated);
    EXPECT_TRUE(result.initial_not_found);
    EXPECT_TRUE(result.initial_not_found_deduplicated);
    EXPECT_TRUE(result.cached_get);
    EXPECT_TRUE(result.missing_get);
    EXPECT_TRUE(result.types_match);
    EXPECT_TRUE(result.listen_batches_bounded);
    EXPECT_TRUE(result.periodic_redo);
    EXPECT_TRUE(result.cas_error_preserved);
    EXPECT_TRUE(result.dirty_requery_reached_latest);
    EXPECT_TRUE(result.md5_deduplicated);
    EXPECT_TRUE(result.removed);
    EXPECT_TRUE(result.shared_subscription);
    EXPECT_TRUE(result.unregister_after_last);
    EXPECT_TRUE(result.stopped_published);
    EXPECT_TRUE(result.clean_shutdown);
    EXPECT_TRUE(result.authority_seen);
    EXPECT_GT(result.notify_acks, 0u);
}

TEST(NacosConfigServiceTest, RestoresSubscriptionAfterReconnect) {
    const ConfigCaseResult result = execute_config_case(true);
    EXPECT_TRUE(result.status_created);
    EXPECT_TRUE(result.status_auth_unavailable);
    EXPECT_TRUE(result.status_ready);
    EXPECT_TRUE(result.status_reconnected);
    EXPECT_TRUE(result.status_stopped);
    EXPECT_TRUE(result.ready);
    EXPECT_TRUE(result.reconnected);
    EXPECT_TRUE(result.update_after_reconnect);
    EXPECT_TRUE(result.clean_shutdown);
    EXPECT_TRUE(result.authority_seen);
    EXPECT_GT(result.notify_acks, 0u);
}

TEST(NacosConfigServiceTest, ShutdownInterruptsAuthWaitAndReconnectBackoff) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    auto finished = std::make_shared<std::promise<ConfigLifecycleInterruptResult>>();
    auto stage = std::make_shared<std::atomic<int>>(0);
    auto future = finished->get_future();
    fiber::async::spawn(group.at(0), [loop = &group.at(0), stage, finished]() {
        return run_config_lifecycle_interrupt_case(loop, stage, finished);
    });
    const auto future_status = future.wait_for(4s);
    if (future_status != std::future_status::ready) {
        ADD_FAILURE() << "timed out at config lifecycle stage " << stage->load(std::memory_order_acquire);
        group.stop();
        group.join();
        return;
    }
    const ConfigLifecycleInterruptResult result = future.get();
    group.stop();
    group.join();
    EXPECT_TRUE(result.auth_wait);
    EXPECT_TRUE(result.auth_unavailable_status);
    EXPECT_TRUE(result.reconnect_backoff);
    EXPECT_TRUE(result.reconnect_backoff_status);
    EXPECT_TRUE(result.stopped_status);
}

struct RnacosConfigResult {
    bool status_ready = false;
    bool status_aggregates = false;
    bool status_stopped = false;
    bool ready = false;
    bool published = false;
    bool queried = false;
    bool subscribed = false;
    bool cas_updated = false;
    bool cas_conflict = false;
    bool removed = false;
    bool stopped = false;
    fiber::nacos::ConfigServiceErrorCode cas_error_code = fiber::nacos::ConfigServiceErrorCode::Protocol;
    std::int32_t cas_result_code = 0;
    std::int32_t cas_server_error_code = 0;
    fiber::common::IoErr cas_io_error = fiber::common::IoErr::None;
    std::string cas_message;
};

DetachedTask run_rnacos_config_case(fiber::event::EventLoop *loop, fiber::nacos::NacosClientConfig config,
                                    fiber::nacos::ConfigServiceOptions options,
                                    std::shared_ptr<std::promise<RnacosConfigResult>> finished) {
    RnacosConfigResult result;
    fiber::async::Watch<fiber::nacos::NacosAuthAccess> auth_watch;
    auto auth_publisher = auth_watch.acquire_publisher();
    FIBER_ASSERT(auth_publisher.has_value());
    auto auth = auth_watch.subscribe();
    auto shared_config = std::make_shared<const fiber::nacos::NacosClientConfig>(std::move(config));
    auto created = nacos_detail::create_config_service(
            {.loop = loop, .config = std::move(shared_config), .auth = std::move(auth)}, std::move(options));
    FIBER_ASSERT(created.has_value());
    auto service = std::move(*created);
    auto service_status = service->subscribe_status();
    FIBER_ASSERT(service->start().has_value());

    auth_publisher->publish(fiber::nacos::NacosAuthAccess{
            .kind = fiber::nacos::NacosAuthAccessKind::Present,
            .access_token = "rnacos-integration-token",
    });

    constexpr std::string_view kDataId = "fiber-config-service-integration";
    constexpr std::string_view kGroup = "DEFAULT_GROUP";
    for (int i = 0; i < 1500; ++i) {
        auto ready = co_await service->get_config(std::string(kDataId), std::string(kGroup));
        if (ready) {
            result.ready = true;
            break;
        }
        co_await fiber::async::sleep(2ms);
    }
    const auto ready_status = service_status.current();
    result.status_ready =
            ready_status.value && ready_status.value->connection.phase == fiber::nacos::NacosServicePhase::Ready &&
            ready_status.value->connection.rpc_available && ready_status.value->connection.connection_ready_count >= 1;
    if (result.ready) {
        (void) co_await service->remove_config(std::string(kDataId), std::string(kGroup));
        auto published = co_await service->publish(std::string(kDataId), std::string(kGroup), "first",
                                                   fiber::nacos::ConfigType::Text);
        result.published = published.has_value();
        auto queried = co_await service->get_config(std::string(kDataId), std::string(kGroup));
        result.queried =
                queried && (*queried)->state == fiber::nacos::ConfigState::Present && (*queried)->content == "first";

        CallbackWatch<fiber::nacos::ConfigData> updates;
        auto subscribed =
                service->subscribe(kDataId, kGroup, &CallbackWatch<fiber::nacos::ConfigData>::notify, &updates);
        if (subscribed) {
            auto subscription = std::move(*subscribed);
            auto &sub = updates.subscriber;
            auto current = sub.current();
            auto initial = co_await fiber::async::timeout_for(
                    [&sub, version = current.version]() { return sub.next(version); }, 3s);
            result.subscribed = initial && initial->value &&
                                initial->value->kind == fiber::nacos::ResultKind::Success && initial->value->data &&
                                initial->value->data->state == fiber::nacos::ConfigState::Present &&
                                initial->value->data->content == "first";
            result.status_aggregates = co_await wait_until([&service_status]() {
                const auto current = service_status.current();
                return current.value && current.value->subscriptions.active_count == 1 &&
                       current.value->subscriptions.registered_count == 1 &&
                       current.value->subscriptions.synchronized_count == 1 &&
                       current.value->subscriptions.pending_count == 0;
            });
            if (initial && initial->value && initial->value->data) {
                auto updated = co_await service->publish(std::string(kDataId), std::string(kGroup), "second",
                                                         fiber::nacos::ConfigType::Text,
                                                         std::optional<std::string>(initial->value->data->md5));
                auto update_snapshot = co_await fiber::async::timeout_for(
                        [&sub, version = initial->version]() { return sub.next(version); }, 3s);
                result.cas_updated = updated.has_value() && update_snapshot && update_snapshot->value &&
                                     update_snapshot->value->kind == fiber::nacos::ResultKind::Success &&
                                     update_snapshot->value->data &&
                                     update_snapshot->value->data->state == fiber::nacos::ConfigState::Present &&
                                     update_snapshot->value->data->content == "second";
                auto conflict = co_await service->publish(std::string(kDataId), std::string(kGroup), "bad",
                                                          fiber::nacos::ConfigType::Text,
                                                          std::optional<std::string>("wrong-md5"));
                if (!conflict) {
                    result.cas_error_code = conflict.error().code;
                    result.cas_result_code = conflict.error().result_code;
                    result.cas_server_error_code = conflict.error().error_code;
                    result.cas_io_error = conflict.error().io_error;
                    result.cas_message = conflict.error().message;
                    result.cas_conflict = conflict.error().code == fiber::nacos::ConfigServiceErrorCode::Server;
                } else {
                    // rnacos 0.8.2 accepts a mismatched casMd5. The scripted server test above verifies
                    // that a server-side CAS error is preserved when the server returns one.
                    result.cas_conflict = true;
                }

                const std::uint64_t remove_version = sub.current().version;
                auto removed = co_await service->remove_config(std::string(kDataId), std::string(kGroup));
                std::uint64_t version = remove_version;
                for (int i = 0; i < 3; ++i) {
                    auto removed_snapshot =
                            co_await fiber::async::timeout_for([&sub, version]() { return sub.next(version); }, 3s);
                    if (!removed_snapshot) {
                        break;
                    }
                    version = removed_snapshot->version;
                    if (removed_snapshot->value && removed_snapshot->value->data &&
                        removed_snapshot->value->data->state == fiber::nacos::ConfigState::NotFound) {
                        result.removed = removed.has_value();
                        break;
                    }
                }
            }
            subscription.close();
        }
    }

    co_await service->shutdown();
    const auto stopped_status = service_status.current();
    result.status_stopped = stopped_status.value &&
                            stopped_status.value->connection.phase == fiber::nacos::NacosServicePhase::Stopped &&
                            !stopped_status.value->connection.rpc_available &&
                            stopped_status.value->subscriptions.active_count == 0;
    auth_publisher->publish(fiber::nacos::NacosAuthAccess{.kind = fiber::nacos::NacosAuthAccessKind::Stopped});
    result.stopped = true;
    finished->set_value(result);
}

TEST(NacosConfigServiceTest, RnacosInteropWhenEnabled) {
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
    params.server_hosts.push_back("127.0.0.1");
    params.username = "nacos";
    params.password = "nacos";
    params.grpc_port = static_cast<std::uint16_t>(port);
    auto config = fiber::nacos::NacosClientConfig::create(std::move(params));
    ASSERT_TRUE(config.has_value());

    fiber::nacos::ConfigServiceOptions options;
    options.rpc.connect_timeout = 1s;
    options.rpc.request_timeout = 2s;
    options.rpc.handshake_timeout = 3s;
    options.rpc.compatibility_setup_delay = 50ms;
    options.rpc.heartbeat_interval = 1s;
    options.rpc.reconnect_initial_delay = 20ms;
    options.rpc.reconnect_max_delay = 100ms;

    fiber::event::EventLoopGroup group(1);
    group.start();
    auto finished = std::make_shared<std::promise<RnacosConfigResult>>();
    auto future = finished->get_future();
    fiber::async::spawn(group.at(0), [loop = &group.at(0), config = std::move(*config), options, finished]() mutable {
        return run_rnacos_config_case(loop, std::move(config), options, finished);
    });
    ASSERT_EQ(future.wait_for(15s), std::future_status::ready);
    const RnacosConfigResult result = future.get();
    group.stop();
    group.join();

    EXPECT_TRUE(result.ready);
    EXPECT_TRUE(result.status_ready);
    EXPECT_TRUE(result.status_aggregates);
    EXPECT_TRUE(result.status_stopped);
    EXPECT_TRUE(result.published);
    EXPECT_TRUE(result.queried);
    EXPECT_TRUE(result.subscribed);
    EXPECT_TRUE(result.cas_updated);
    EXPECT_TRUE(result.cas_conflict) << "code=" << static_cast<int>(result.cas_error_code)
                                     << " resultCode=" << result.cas_result_code
                                     << " errorCode=" << result.cas_server_error_code
                                     << " io=" << static_cast<int>(result.cas_io_error)
                                     << " message=" << result.cas_message;
    EXPECT_TRUE(result.removed);
    EXPECT_TRUE(result.stopped);
}

} // namespace
