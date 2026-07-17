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

#include <async/Sleep.h>
#include <async/Spawn.h>
#include <async/Timeout.h>
#include <async/WaitGroup.h>
#include <event/EventLoopGroup.h>
#include <fiber/nacos/ConfigService.h>
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

#include "../src/config/ConfigServiceImpl.h"

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;
namespace dto = fiber::nacos::dto;
namespace nacos_detail = fiber::nacos::detail;
namespace proto = fiber::nacos::proto;

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

class ScriptedConfigServer {
public:
    ScriptedConfigServer(fiber::event::EventLoop &loop, bool reset_after_first_listen = false) :
        loop_(&loop), reset_after_first_listen_(reset_after_first_listen),
        connections_to_serve_(reset_after_first_listen ? 2 : 1),
        handler_([this](fiber::http::HttpExchange &exchange) { return handle(exchange); }),
        factory_(http_options_, handler_), listener_(loop) {
        push_publisher_ = push_watch_.acquire_publisher();
        FIBER_ASSERT(push_publisher_.has_value());
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
    [[nodiscard]] std::size_t listen_count() const noexcept { return listen_count_; }
    [[nodiscard]] std::size_t unlisten_count() const noexcept { return unlisten_count_; }
    [[nodiscard]] std::size_t setup_count() const noexcept { return setup_count_; }
    [[nodiscard]] std::size_t query_count() const noexcept { return query_count_; }
    [[nodiscard]] std::size_t notify_ack_count() const noexcept { return notify_ack_count_; }
    [[nodiscard]] std::size_t max_listen_contexts() const noexcept { return max_listen_contexts_; }
    [[nodiscard]] const std::vector<std::string> &published_types() const noexcept { return published_types_; }

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
                const bool differs = exists_ && (!context.md5.is_present() || context.md5.value() != md5_);
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
        fiber::grpc::GrpcFrameReader reader(1024 * 1024);
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
                notify.request_id.set_present("notify-" + std::to_string(sent_sequence));
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
    fiber::http::Http2HpackEncodeCatalog catalog_;
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
};

struct ConfigCaseResult {
    bool ready = false;
    bool initial_present = false;
    bool cached_get = false;
    bool missing_get = false;
    bool types_match = false;
    bool listen_batches_bounded = false;
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
    std::size_t notify_acks = 0;
};

DetachedTask drive_connection(nacos_detail::NacosGrpcConnection *connection, fiber::async::WaitGroup *group) {
    co_await connection->run();
    group->done();
}

DetachedTask drive_service(nacos_detail::ConfigServiceImpl *service, fiber::async::WaitGroup *group) {
    co_await service->run();
    group->done();
}

fiber::async::Task<bool> wait_ready(nacos_detail::NacosGrpcConnection &connection,
                                    std::uint64_t minimum_generation = 1) {
    auto states = connection.subscribe_state();
    auto current = states.current();
    std::uint64_t version = current.version;
    for (;;) {
        if (current.value && current.value->state == nacos_detail::NacosGrpcConnectionState::Ready &&
            current.value->generation >= minimum_generation) {
            co_return true;
        }
        auto next = co_await fiber::async::timeout_for([&states, version]() { return states.next(version); }, 2s);
        if (!next) {
            co_return false;
        }
        current = std::move(*next);
        version = current.version;
    }
}

DetachedTask run_config_case(fiber::event::EventLoop *loop, ScriptedConfigServer *server,
                             fiber::nacos::NacosClientConfig config, fiber::nacos::NacosClientOptions options,
                             bool reconnect, std::shared_ptr<std::promise<ConfigCaseResult>> finished) {
    ConfigCaseResult result;
    nacos_detail::NacosGrpcConnection connection(*loop, config, options);
    nacos_detail::ConfigServiceImpl service(*loop, config, options, connection);
    std::vector<fiber::nacos::Subscription<fiber::nacos::ConfigData>> batched_subscriptions;
    if (!reconnect) {
        for (int i = 0; i < 5; ++i) {
            auto subscription = service.subscribe("batch-" + std::to_string(i), "group");
            if (subscription) {
                batched_subscriptions.push_back(std::move(*subscription));
            }
        }
    }
    fiber::async::WaitGroup tasks;
    tasks.add(2);
    fiber::async::spawn(*loop, [&connection, &tasks]() { return drive_connection(&connection, &tasks); });
    fiber::async::spawn(*loop, [&service, &tasks]() { return drive_service(&service, &tasks); });

    fiber::nacos::NacosAuthSnapshot auth;
    auth.state = fiber::nacos::NacosAuthState::Ready;
    auth.access_token = "token";
    auth.expires_at = loop->now() + 1min;
    connection.notify_auth(auth);
    result.ready = co_await wait_ready(connection);

    if (result.ready && !reconnect) {
        for (int i = 0; i < 100 && server->listen_count() < 3; ++i) {
            co_await fiber::async::sleep(2ms);
        }
        result.listen_batches_bounded = server->listen_count() >= 3 && server->max_listen_contexts() <= 2;
        for (auto &subscription: batched_subscriptions) {
            subscription.close();
        }

        const fiber::nacos::ConfigType types[] = {
                fiber::nacos::ConfigType::Json,       fiber::nacos::ConfigType::Text, fiber::nacos::ConfigType::Yaml,
                fiber::nacos::ConfigType::Properties, fiber::nacos::ConfigType::Xml,  fiber::nacos::ConfigType::Html,
        };
        for (std::size_t i = 0; i < std::size(types); ++i) {
            auto published = co_await service.publish("data", "group", "type-" + std::to_string(i), types[i]);
            if (!published) {
                result.ready = false;
                break;
            }
        }
        result.types_match = server->published_types() ==
                             std::vector<std::string>({"json", "text", "yaml", "properties", "xml", "html"});

        auto missing = co_await service.get_config("missing", "group");
        result.missing_get = missing && !missing->has_value();

        auto subscribed = service.subscribe("data", "group");
        auto stopped_subscription = service.subscribe("stopped", "group");
        if (subscribed && stopped_subscription) {
            auto subscription = std::move(*subscribed);
            auto &sub = subscription.subscriber();
            auto initial = sub.current();
            auto next = co_await fiber::async::timeout_for(
                    [&sub, version = initial.version]() { return sub.next(version); }, 2s);
            result.initial_present = next && next->value && next->value->kind == fiber::nacos::ResultKind::Success &&
                                     next->value->data &&
                                     next->value->data->state == fiber::nacos::ConfigState::Present &&
                                     next->value->data->content == "type-5";
            std::uint64_t version = next ? next->version : initial.version;

            auto cached = co_await service.get_config("data", "group");
            result.cached_get = cached && cached->has_value() && (*cached)->content == "type-5";

            const std::size_t listen_before_shared = server->listen_count();
            auto shared = service.subscribe("data", "group");
            if (shared) {
                auto shared_subscription = std::move(*shared);
                const auto replay = shared_subscription.subscriber().current();
                result.shared_subscription = replay.value && replay.value->data &&
                                             replay.value->data->state == fiber::nacos::ConfigState::Present &&
                                             replay.value->data->content == "type-5" &&
                                             server->listen_count() == listen_before_shared;

                const std::size_t query_before = server->query_count();
                auto first = co_await service.publish("data", "group", "first", fiber::nacos::ConfigType::Text,
                                                      std::optional<std::string>("m6"));
                for (int i = 0; i < 100 && server->query_count() == query_before; ++i) {
                    co_await fiber::async::sleep(2ms);
                }
                auto second = co_await service.publish("data", "group", "second", fiber::nacos::ConfigType::Text);
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
                auto same = co_await service.publish("data", "group", "second", fiber::nacos::ConfigType::Text);
                auto duplicate = co_await fiber::async::timeout_for(
                        [&sub, dedup_version]() { return sub.next(dedup_version); }, 150ms);
                result.md5_deduplicated =
                        same.has_value() && !duplicate && duplicate.error() == fiber::common::IoErr::TimedOut;

                auto conflict = co_await service.publish("data", "group", "bad", fiber::nacos::ConfigType::Text,
                                                         std::optional<std::string>("wrong"));
                result.cas_error_preserved =
                        !conflict && conflict.error().code == fiber::nacos::ConfigServiceErrorCode::Server &&
                        conflict.error().error_code == dto::resp::ConfigQueryResponse::kConfigQueryConflict;

                auto removed = co_await service.remove_config("data", "group");
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

            service.shutdown();
            // "stopped" was never synced: shutdown publishes Closed for it.
            result.stopped_published = stopped_subscription->closed();
            stopped_subscription->close();
        } else {
            service.shutdown();
        }
    } else if (result.ready) {
        auto subscribed = service.subscribe("data", "group");
        if (subscribed) {
            auto subscription = std::move(*subscribed);
            auto &sub = subscription.subscriber();
            auto current = sub.current();
            auto initial = co_await fiber::async::timeout_for(
                    [&sub, version = current.version]() { return sub.next(version); }, 2s);
            if (initial && initial->value && initial->value->kind == fiber::nacos::ResultKind::Success) {
                const std::uint64_t version = initial->version;
                result.reconnected = co_await wait_ready(connection, 2);
                for (int i = 0; i < 100 && server->listen_count() < 2; ++i) {
                    co_await fiber::async::sleep(2ms);
                }
                result.reconnected = result.reconnected && server->listen_count() >= 2 && server->setup_count() >= 2;
                auto published =
                        co_await service.publish("data", "group", "after-reconnect", fiber::nacos::ConfigType::Text);
                auto updated = co_await fiber::async::timeout_for([&sub, version]() { return sub.next(version); }, 2s);
                result.update_after_reconnect = published.has_value() && updated && updated->value &&
                                                updated->value->kind == fiber::nacos::ResultKind::Success &&
                                                updated->value->data &&
                                                updated->value->data->state == fiber::nacos::ConfigState::Present &&
                                                updated->value->data->content == "after-reconnect";
            }
            subscription.close();
        }
        service.shutdown();
    } else {
        service.shutdown();
    }

    connection.shutdown();
    co_await tasks.join();
    result.clean_shutdown = true;
    result.notify_acks = server->notify_ack_count();
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
    options.grpc_compatibility_setup_delay = 10ms;
    options.grpc_heartbeat_interval = 1s;
    options.grpc_reconnect_initial_delay = 10ms;
    options.grpc_reconnect_max_delay = 50ms;
    options.config_subscription_redo_interval = 5s;
    options.max_inbound_grpc_message_bytes = 1024 * 1024;
    options.max_config_content_bytes = 1024 * 1024;
    options.max_push_response_bytes = 64 * 1024;
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

TEST(NacosConfigServiceTest, UnarySubscriptionDedupDirtyUnregisterAndShutdown) {
    const ConfigCaseResult result = execute_config_case(false);
    EXPECT_TRUE(result.ready);
    EXPECT_TRUE(result.initial_present);
    EXPECT_TRUE(result.cached_get);
    EXPECT_TRUE(result.missing_get);
    EXPECT_TRUE(result.types_match);
    EXPECT_TRUE(result.listen_batches_bounded);
    EXPECT_TRUE(result.cas_error_preserved);
    EXPECT_TRUE(result.dirty_requery_reached_latest);
    EXPECT_TRUE(result.md5_deduplicated);
    EXPECT_TRUE(result.removed);
    EXPECT_TRUE(result.shared_subscription);
    EXPECT_TRUE(result.unregister_after_last);
    EXPECT_TRUE(result.stopped_published);
    EXPECT_TRUE(result.clean_shutdown);
    EXPECT_GT(result.notify_acks, 0u);
}

TEST(NacosConfigServiceTest, RestoresSubscriptionAfterReconnect) {
    const ConfigCaseResult result = execute_config_case(true);
    EXPECT_TRUE(result.ready);
    EXPECT_TRUE(result.reconnected);
    EXPECT_TRUE(result.update_after_reconnect);
    EXPECT_TRUE(result.clean_shutdown);
    EXPECT_GT(result.notify_acks, 0u);
}

struct RnacosConfigResult {
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
                                    fiber::nacos::NacosClientOptions options,
                                    std::shared_ptr<std::promise<RnacosConfigResult>> finished) {
    RnacosConfigResult result;
    nacos_detail::NacosGrpcConnection connection(*loop, config, options);
    nacos_detail::ConfigServiceImpl service(*loop, config, options, connection);
    fiber::async::WaitGroup tasks;
    tasks.add(2);
    fiber::async::spawn(*loop, [&connection, &tasks]() { return drive_connection(&connection, &tasks); });
    fiber::async::spawn(*loop, [&service, &tasks]() { return drive_service(&service, &tasks); });

    fiber::nacos::NacosAuthSnapshot auth;
    auth.state = fiber::nacos::NacosAuthState::Ready;
    auth.access_token = "rnacos-integration-token";
    auth.expires_at = loop->now() + 1min;
    connection.notify_auth(auth);
    result.ready = co_await wait_ready(connection);

    constexpr std::string_view kDataId = "fiber-config-service-integration";
    constexpr std::string_view kGroup = "DEFAULT_GROUP";
    if (result.ready) {
        (void) co_await service.remove_config(std::string(kDataId), std::string(kGroup));
        auto published = co_await service.publish(std::string(kDataId), std::string(kGroup), "first",
                                                  fiber::nacos::ConfigType::Text);
        result.published = published.has_value();
        auto queried = co_await service.get_config(std::string(kDataId), std::string(kGroup));
        result.queried = queried && queried->has_value() && (*queried)->content == "first";

        auto subscribed = service.subscribe(kDataId, kGroup);
        if (subscribed) {
            auto subscription = std::move(*subscribed);
            auto &sub = subscription.subscriber();
            auto current = sub.current();
            auto initial = co_await fiber::async::timeout_for(
                    [&sub, version = current.version]() { return sub.next(version); }, 3s);
            result.subscribed = initial && initial->value &&
                                initial->value->kind == fiber::nacos::ResultKind::Success && initial->value->data &&
                                initial->value->data->state == fiber::nacos::ConfigState::Present &&
                                initial->value->data->content == "first";
            if (initial && initial->value && initial->value->data) {
                auto updated = co_await service.publish(std::string(kDataId), std::string(kGroup), "second",
                                                        fiber::nacos::ConfigType::Text,
                                                        std::optional<std::string>(initial->value->data->md5));
                auto update_snapshot = co_await fiber::async::timeout_for(
                        [&sub, version = initial->version]() { return sub.next(version); }, 3s);
                result.cas_updated = updated.has_value() && update_snapshot && update_snapshot->value &&
                                     update_snapshot->value->kind == fiber::nacos::ResultKind::Success &&
                                     update_snapshot->value->data &&
                                     update_snapshot->value->data->state == fiber::nacos::ConfigState::Present &&
                                     update_snapshot->value->data->content == "second";
                auto conflict = co_await service.publish(std::string(kDataId), std::string(kGroup), "bad",
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
                auto removed = co_await service.remove_config(std::string(kDataId), std::string(kGroup));
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

    service.shutdown();
    connection.shutdown();
    co_await tasks.join();
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
    params.server_ips.push_back(fiber::net::IpAddress::loopback_v4());
    params.username = "nacos";
    params.password = "nacos";
    params.grpc_port = static_cast<std::uint16_t>(port);
    auto config = fiber::nacos::NacosClientConfig::create(std::move(params));
    ASSERT_TRUE(config.has_value());

    fiber::nacos::NacosClientOptions options;
    options.grpc_connect_timeout = 1s;
    options.grpc_request_timeout = 2s;
    options.grpc_handshake_timeout = 3s;
    options.grpc_compatibility_setup_delay = 50ms;
    options.grpc_heartbeat_interval = 1s;
    options.grpc_reconnect_initial_delay = 20ms;
    options.grpc_reconnect_max_delay = 100ms;

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
