#include "NacosGrpcConnection.h"

#include <algorithm>
#include <charconv>
#include <memory>
#include <utility>
#include <vector>

#include <async/Timeout.h>
#include <common/Assert.h>
#include <fiber/nacos/dto/Internal.h>
#include <grpc/GrpcClient.h>
#include <grpc/GrpcStream.h>
#include <net/SocketAddress.h>

namespace fiber::nacos::detail {
namespace {

NacosRpcError transport_error(common::IoErr error, std::string message = {}) {
    return NacosRpcError{
            .code = NacosRpcErrorCode::Transport,
            .io_error = error,
            .message = std::move(message),
    };
}

NacosRpcError shutdown_error() {
    return NacosRpcError{
            .code = NacosRpcErrorCode::Shutdown,
            .io_error = common::IoErr::Canceled,
    };
}

NacosRpcError protocol_error(std::string message) {
    return NacosRpcError{
            .code = NacosRpcErrorCode::Protocol,
            .io_error = common::IoErr::Invalid,
            .message = std::move(message),
    };
}

bool ends_with_request(std::string_view type) noexcept {
    constexpr std::string_view suffix = "Request";
    return type.size() >= suffix.size() && type.substr(type.size() - suffix.size()) == suffix;
}

std::chrono::milliseconds remaining_until(std::chrono::steady_clock::time_point deadline,
                                          std::chrono::steady_clock::time_point now) noexcept {
    if (now >= deadline) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}

} // namespace

struct NacosGrpcConnection::Generation {
    struct Outbound {
        proto::Payload payload;
        std::size_t bytes = 0;
        bool close_after = false;
    };

    Generation(NacosGrpcConnection &owner, Endpoint endpoint) :
        owner(&owner), endpoint(std::move(endpoint)), outbound_slots(owner.options_->max_push_response_queue) {
        stop_publisher = stop_watch.acquire_publisher();
        outbound_publisher = outbound_watch.acquire_publisher();
        FIBER_ASSERT(stop_publisher.has_value());
        FIBER_ASSERT(outbound_publisher.has_value());
    }

    bool enqueue(proto::Payload payload, bool close_after) noexcept {
        const std::size_t bytes = payload.ByteSizeLong();
        if (queue_size == outbound_slots.size() || bytes > owner->options_->max_push_response_bytes - queue_bytes) {
            return false;
        }
        outbound_slots[queue_tail].emplace(Outbound{
                .payload = std::move(payload),
                .bytes = bytes,
                .close_after = close_after,
        });
        queue_tail = (queue_tail + 1) % outbound_slots.size();
        ++queue_size;
        queue_bytes += bytes;
        outbound_publisher->publish(++outbound_sequence);
        return true;
    }

    std::optional<Outbound> pop() noexcept {
        if (queue_size == 0) {
            return std::nullopt;
        }
        auto result = std::move(outbound_slots[queue_head]);
        outbound_slots[queue_head].reset();
        queue_head = (queue_head + 1) % outbound_slots.size();
        --queue_size;
        queue_bytes -= result->bytes;
        return result;
    }

    void stop(common::IoErr reason = common::IoErr::Canceled) noexcept {
        if (stopping) {
            return;
        }
        stopping = true;
        stop_reason = reason;
        if (stream.valid()) {
            stream.cancel(reason);
        }
        stop_publisher->publish(true);
        outbound_publisher->publish(++outbound_sequence);
        owner->wake_control();
    }

    NacosGrpcConnection *owner = nullptr;
    Endpoint endpoint;
    std::unique_ptr<grpc::GrpcClient> client;
    std::unique_ptr<NacosRequester> requester;
    mem::BufPool stream_pool;
    grpc::GrpcStream stream;
    async::WaitGroup tasks;
    async::Watch<bool> stop_watch{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher;
    async::Watch<std::uint64_t> outbound_watch{0};
    std::optional<async::Watch<std::uint64_t>::Publisher> outbound_publisher;
    std::vector<std::optional<Outbound>> outbound_slots;
    std::size_t queue_head = 0;
    std::size_t queue_tail = 0;
    std::size_t queue_size = 0;
    std::size_t queue_bytes = 0;
    std::uint64_t outbound_sequence = 0;
    common::IoErr stop_reason = common::IoErr::Canceled;
    bool stopping = false;
    bool reached_ready = false;
};

NacosGrpcConnection::NacosGrpcConnection(event::EventLoop &loop, const NacosClientConfig &config,
                                         const NacosClientOptions &options) :
    loop_(&loop), config_(&config), options_(&options) {
    control_publisher_ = control_watch_.acquire_publisher();
    state_publisher_ = state_watch_.acquire_publisher();
    FIBER_ASSERT(control_publisher_.has_value());
    FIBER_ASSERT(state_publisher_.has_value());
}

NacosGrpcConnection::~NacosGrpcConnection() {
    FIBER_ASSERT(!run_active_);
    FIBER_ASSERT(active_generation_ == nullptr);
    FIBER_ASSERT(snapshot_.state == NacosGrpcConnectionState::Created ||
                 snapshot_.state == NacosGrpcConnectionState::Stopped);
}

void NacosGrpcConnection::notify_auth(const NacosAuthSnapshot &snapshot) {
    FIBER_ASSERT(loop_->in_loop());
    control_.auth = snapshot;
    control_.has_auth = true;
    control_publisher_->publish(control_);
}

void NacosGrpcConnection::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (control_.stopping) {
        return;
    }
    control_.stopping = true;
    control_publisher_->publish(control_);
    if (active_generation_) {
        active_generation_->stop();
    }
}

void NacosGrpcConnection::set_push_handler(NacosPushHandler handler) noexcept {
    FIBER_ASSERT(snapshot_.state == NacosGrpcConnectionState::Created);
    push_handler_ = handler;
}

NacosGrpcConnection::StateSubscriber NacosGrpcConnection::subscribe_state() { return state_watch_.subscribe(); }

std::optional<NacosGrpcConnection::RequestLease> NacosGrpcConnection::acquire_requester() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (snapshot_.state != NacosGrpcConnectionState::Ready || active_generation_ == nullptr ||
        active_generation_->stopping || active_requester_ == nullptr) {
        return std::nullopt;
    }
    active_generation_->tasks.add();
    return RequestLease{
            .generation = active_generation_,
            .requester = active_requester_,
    };
}

void NacosGrpcConnection::release_requester(Generation *generation) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(generation != nullptr);
    generation->tasks.done();
}

bool NacosGrpcConnection::auth_ready() const noexcept {
    return control_.has_auth && control_.auth.state == NacosAuthState::Ready && !control_.auth.access_token.empty() &&
           event::EventLoop::current().now() < control_.auth.expires_at;
}

std::expected<NacosPayloadMetadata, NacosRpcError> NacosGrpcConnection::current_metadata() const noexcept {
    if (!auth_ready()) {
        return std::unexpected(NacosRpcError{
                .code = NacosRpcErrorCode::AuthenticationUnavailable,
                .io_error = common::IoErr::Permission,
        });
    }
    if (active_client_ip_.empty()) {
        return std::unexpected(protocol_error("Nacos gRPC local address is unavailable"));
    }
    return NacosPayloadMetadata{
            .client_ip = active_client_ip_,
            .client_version = config_->client_version(),
            .namespace_id = config_->namespace_id(),
            .access_token = control_.auth.access_token,
    };
}

void NacosGrpcConnection::publish_state(NacosGrpcConnectionState state, std::optional<NacosRpcError> error,
                                        std::optional<std::size_t> server_index) {
    FIBER_ASSERT(loop_->in_loop());
    snapshot_.state = state;
    if (error) {
        snapshot_.last_error = std::move(*error);
    } else {
        snapshot_.last_error = {};
    }
    if (server_index) {
        snapshot_.server_index = *server_index;
    }
    state_publisher_->publish(snapshot_);
}

void NacosGrpcConnection::wake_control() { control_publisher_->publish(control_); }

std::string NacosGrpcConnection::authority(const Endpoint &endpoint) const {
    std::string result;
    if (endpoint.ip.is_v6()) {
        result.push_back('[');
        result.append(endpoint.ip.to_string());
        result.push_back(']');
    } else {
        result = endpoint.ip.to_string();
    }
    result.push_back(':');
    result.append(std::to_string(endpoint.port));
    return result;
}

std::chrono::milliseconds NacosGrpcConnection::jittered(std::chrono::milliseconds delay) noexcept {
    random_state_ ^= random_state_ << 13;
    random_state_ ^= random_state_ >> 7;
    random_state_ ^= random_state_ << 17;
    const auto spread = delay / 5;
    if (spread <= std::chrono::milliseconds::zero()) {
        return delay;
    }
    const std::uint64_t width = static_cast<std::uint64_t>(spread.count()) * 2 + 1;
    const auto offset = static_cast<std::int64_t>(random_state_ % width) - spread.count();
    return delay + std::chrono::milliseconds(offset);
}

void NacosGrpcConnection::save_redirect(const dto::req::ConnectResetRequest &request) noexcept {
    if (!request.server_ip.is_present() || request.server_ip.value().empty()) {
        return;
    }
    net::IpAddress ip;
    if (!net::IpAddress::parse(request.server_ip.value(), ip) || ip.is_unspecified() || ip.is_multicast()) {
        return;
    }
    std::uint16_t port = config_->grpc_port();
    if (request.server_port.is_present() && !request.server_port.value().empty()) {
        unsigned parsed = 0;
        const std::string_view text = request.server_port.value();
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size() || parsed == 0 || parsed > 65535) {
            return;
        }
        port = static_cast<std::uint16_t>(parsed);
    }
    redirect_ = Endpoint{
            .ip = ip,
            .port = port,
            .server_index = std::nullopt,
    };
}

async::DetachedTask NacosGrpcConnection::drive_client(Generation *generation) noexcept {
    auto result = co_await generation->client->run();
    if (!result && !generation->stopping) {
        generation->stop(result.error());
    } else if (!generation->stopping) {
        generation->stop(common::IoErr::ConnReset);
    }
    generation->tasks.done();
}

async::DetachedTask NacosGrpcConnection::run_writer(Generation *generation) noexcept {
    auto subscriber = generation->outbound_watch.subscribe();
    std::uint64_t version = subscriber.current().version;
    while (!generation->stopping) {
        while (!generation->stopping) {
            auto outbound = generation->pop();
            if (!outbound) {
                break;
            }
            auto result = co_await generation->stream.write(outbound->payload);
            if (!result) {
                generation->stop(result.error());
                break;
            }
            if (outbound->close_after) {
                generation->stop(common::IoErr::Canceled);
                break;
            }
        }
        if (generation->stopping) {
            break;
        }
        auto next = co_await subscriber.next(version);
        version = next.version;
    }
    generation->tasks.done();
}

async::DetachedTask NacosGrpcConnection::monitor_control(Generation *generation) noexcept {
    auto subscriber = control_watch_.subscribe();
    std::uint64_t version = subscriber.current().version;
    while (!generation->stopping) {
        auto next = co_await subscriber.next(version);
        version = next.version;
        if (generation->stopping) {
            break;
        }
        if (next.value->stopping || next.value->auth.state != NacosAuthState::Ready ||
            next.value->auth.access_token.empty() || event::EventLoop::current().now() >= next.value->auth.expires_at) {
            generation->stop();
            break;
        }
    }
    generation->tasks.done();
}

async::DetachedTask NacosGrpcConnection::run_heartbeat(Generation *generation) noexcept {
    auto stop_subscriber = generation->stop_watch.subscribe();
    std::uint64_t stop_version = stop_subscriber.current().version;
    while (!generation->stopping) {
        auto wait = co_await async::timeout_for(
                [&stop_subscriber, stop_version]() { return stop_subscriber.next(stop_version); },
                options_->grpc_heartbeat_interval);
        if (wait) {
            stop_version = wait->version;
            break;
        }
        if (wait.error() != common::IoErr::TimedOut || generation->stopping) {
            break;
        }
        auto metadata = current_metadata();
        if (!metadata) {
            generation->stop();
            break;
        }
        dto::req::HealthCheckRequest request;
        dto::resp::HealthCheckResponse response;
        mem::BufPool pool;
        (void) co_await generation->requester->request(request, *metadata, pool, response);
    }
    generation->tasks.done();
}

std::expected<bool, NacosRpcError>
NacosGrpcConnection::handle_inbound(Generation &generation, const proto::Payload &payload, bool handshaking) noexcept {
    auto view = validate_payload(payload, options_->max_inbound_grpc_message_bytes);
    if (!view) {
        return std::unexpected(std::move(view.error()));
    }

    if (view->type == dto::req::SetupAckRequest::kTypeName) {
        mem::BufPool pool;
        dto::req::SetupAckRequest request;
        auto parsed = parse_payload_json(*view, pool, request);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        return true;
    }

    auto metadata = current_metadata();
    if (!metadata) {
        return std::unexpected(std::move(metadata.error()));
    }

    if (view->type == dto::req::ClientDetectionRequest::kTypeName) {
        mem::BufPool pool;
        dto::req::ClientDetectionRequest request;
        auto parsed = parse_payload_json(*view, pool, request);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        dto::resp::ClientDetectionResponse response;
        response.request_id = request.request_id;
        auto encoded = encode_payload(response, *metadata, options_->max_inbound_grpc_message_bytes);
        if (!encoded) {
            return std::unexpected(std::move(encoded.error()));
        }
        if (!generation.enqueue(std::move(*encoded), false)) {
            return std::unexpected(NacosRpcError{
                    .code = NacosRpcErrorCode::QueueFull,
                    .io_error = common::IoErr::MessageTooLarge,
                    .message = "Nacos push response queue is full",
            });
        }
        return false;
    }

    if (view->type == dto::req::ConnectResetRequest::kTypeName) {
        mem::BufPool pool;
        dto::req::ConnectResetRequest request;
        auto parsed = parse_payload_json(*view, pool, request);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        save_redirect(request);
        dto::resp::ConnectResetResponse response;
        response.request_id = request.request_id;
        auto encoded = encode_payload(response, *metadata, options_->max_inbound_grpc_message_bytes);
        if (!encoded) {
            return std::unexpected(std::move(encoded.error()));
        }
        if (!generation.enqueue(std::move(*encoded), true)) {
            return std::unexpected(NacosRpcError{
                    .code = NacosRpcErrorCode::QueueFull,
                    .io_error = common::IoErr::MessageTooLarge,
                    .message = "Nacos push response queue is full",
            });
        }
        return false;
    }

    if (push_handler_.callback) {
        auto response = push_handler_.callback(push_handler_.context, payload, *metadata,
                                               options_->max_inbound_grpc_message_bytes);
        if (!response) {
            return std::unexpected(std::move(response.error()));
        }
        auto response_view = validate_payload(*response, options_->max_inbound_grpc_message_bytes);
        if (!response_view) {
            return std::unexpected(std::move(response_view.error()));
        }
        if (!generation.enqueue(std::move(*response), false)) {
            return std::unexpected(NacosRpcError{
                    .code = NacosRpcErrorCode::QueueFull,
                    .io_error = common::IoErr::MessageTooLarge,
                    .message = "Nacos push response queue is full",
            });
        }
        return false;
    }

    if (ends_with_request(view->type)) {
        mem::BufPool pool;
        dto::RequestBase request;
        auto parsed = parse_payload_json(*view, pool, request);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        dto::resp::ErrorResponse response;
        response.result_code = dto::kResponseFail;
        response.error_code = dto::kResponseFail;
        response.message.set_present("unsupported Nacos server request");
        response.request_id = request.request_id;
        auto encoded = encode_payload(response, *metadata, options_->max_inbound_grpc_message_bytes);
        if (!encoded) {
            return std::unexpected(std::move(encoded.error()));
        }
        if (!generation.enqueue(std::move(*encoded), false)) {
            return std::unexpected(NacosRpcError{
                    .code = NacosRpcErrorCode::QueueFull,
                    .io_error = common::IoErr::MessageTooLarge,
                    .message = "Nacos push response queue is full",
            });
        }
    }
    (void) handshaking;
    return false;
}

async::Task<NacosGrpcConnection::AttemptResult> NacosGrpcConnection::run_generation(const Endpoint &endpoint) noexcept {
    Generation generation(*this, endpoint);
    active_generation_ = &generation;

    const auto cleanup = [this, &generation]() -> async::Task<void> {
        generation.stop();
        if (generation.client) {
            co_await generation.client->shutdown();
        }
        co_await generation.tasks.join();
        active_requester_ = nullptr;
        active_client_ip_.clear();
        active_generation_ = nullptr;
    };

    const std::size_t state_server_index = endpoint.server_index.value_or(preferred_server_index_);
    publish_state(NacosGrpcConnectionState::Connecting, std::nullopt, state_server_index);
    grpc::GrpcClient::Options client_options;
    client_options.peer_addr = net::SocketAddress(endpoint.ip, endpoint.port);
    const std::string endpoint_authority = authority(endpoint);
    client_options.authority = endpoint_authority;
    client_options.scheme = "http";
    generation.client = std::make_unique<grpc::GrpcClient>(*loop_, std::move(client_options));

    auto connected = co_await generation.client->connect(options_->grpc_connect_timeout);
    if (!connected) {
        const AttemptResult result{.error = transport_error(connected.error())};
        co_await cleanup();
        co_return result;
    }
    if (control_.stopping || !auth_ready()) {
        const AttemptResult result{.error = shutdown_error()};
        co_await cleanup();
        co_return result;
    }

    if (!options_->client_ip_override.empty()) {
        active_client_ip_ = options_->client_ip_override;
    } else if (generation.client->local_addr()) {
        active_client_ip_ = generation.client->local_addr()->ip().to_string();
    }
    if (active_client_ip_.empty()) {
        const AttemptResult result{.error = protocol_error("Nacos gRPC local address is unavailable")};
        co_await cleanup();
        co_return result;
    }

    generation.tasks.add();
    async::spawn(*loop_, [this, &generation]() { return drive_client(&generation); });
    generation.tasks.add();
    async::spawn(*loop_, [this, &generation]() { return monitor_control(&generation); });

    const auto handshake_deadline = event::EventLoop::current().now() + options_->grpc_handshake_timeout;
    const auto request_timeout = std::min(options_->grpc_request_timeout,
                                          remaining_until(handshake_deadline, event::EventLoop::current().now()));
    if (request_timeout <= std::chrono::milliseconds::zero()) {
        const AttemptResult result{.error = transport_error(common::IoErr::TimedOut, "Nacos handshake timed out")};
        co_await cleanup();
        co_return result;
    }
    generation.requester = std::make_unique<NacosRequester>(*generation.client,
                                                            options_->max_inbound_grpc_message_bytes, request_timeout);
    publish_state(NacosGrpcConnectionState::Checking, std::nullopt, state_server_index);
    auto metadata = current_metadata();
    if (!metadata) {
        const AttemptResult result{.error = std::move(metadata.error())};
        co_await cleanup();
        co_return result;
    }
    dto::req::ServerCheckRequest server_check;
    dto::resp::ServerCheckResponse server_check_response;
    mem::BufPool check_pool;
    auto checked = co_await generation.requester->request(server_check, *metadata, check_pool, server_check_response);
    if (!checked) {
        const AttemptResult result{.error = std::move(checked.error())};
        co_await cleanup();
        co_return result;
    }
    if (!server_check_response.success()) {
        NacosRpcError error{
                .code = NacosRpcErrorCode::Server,
                .result_code = server_check_response.result_code,
                .error_code = server_check_response.error_code,
        };
        if (server_check_response.message.is_present()) {
            error.message.assign(server_check_response.message.value().substr(0, 512));
        }
        const AttemptResult result{.error = std::move(error)};
        co_await cleanup();
        co_return result;
    }

    publish_state(NacosGrpcConnectionState::Handshaking, std::nullopt, state_server_index);
    generation.stream =
            generation.client->open_stream("BiRequestStream", "requestBiStream", generation.stream_pool,
                                           {.max_inbound_message_bytes = options_->max_inbound_grpc_message_bytes});
    auto remaining = remaining_until(handshake_deadline, event::EventLoop::current().now());
    if (remaining <= std::chrono::milliseconds::zero()) {
        const AttemptResult result{.error = transport_error(common::IoErr::TimedOut, "Nacos handshake timed out")};
        co_await cleanup();
        co_return result;
    }
    generation.stream.set_local_deadline(remaining);
    auto stream_result = co_await generation.stream.open();
    if (!stream_result) {
        const AttemptResult result{.error = transport_error(stream_result.error())};
        co_await cleanup();
        co_return result;
    }

    json::JsonObject<std::string_view>::Entry label_entries[] = {
            {.key = "source", .value = "sdk"},
            {.key = "module", .value = "config"},
    };
    dto::req::ConnectionSetupRequest setup;
    setup.client_version.set_present(config_->client_version());
    setup.tenant.set_present(config_->tenant());
    setup.labels.set_present(json::JsonObject<std::string_view>(label_entries, std::size(label_entries)));
    setup.ability_table.set_present(json::JsonObject<bool>());
    metadata = current_metadata();
    if (!metadata) {
        const AttemptResult result{.error = std::move(metadata.error())};
        co_await cleanup();
        co_return result;
    }
    auto setup_payload = encode_payload(setup, *metadata, options_->max_inbound_grpc_message_bytes);
    if (!setup_payload) {
        const AttemptResult result{.error = std::move(setup_payload.error())};
        co_await cleanup();
        co_return result;
    }
    stream_result = co_await generation.stream.write(*setup_payload);
    if (!stream_result) {
        const AttemptResult result{.error = transport_error(stream_result.error())};
        co_await cleanup();
        co_return result;
    }

    generation.tasks.add();
    async::spawn(*loop_, [this, &generation]() { return run_writer(&generation); });

    if (server_check_response.support_ability_negotiation) {
        bool acked = false;
        while (!generation.stopping && !acked) {
            proto::Payload inbound;
            auto read = co_await generation.stream.read(inbound);
            if (!read) {
                const AttemptResult result{.error = transport_error(read.error())};
                co_await cleanup();
                co_return result;
            }
            if (*read == grpc::GrpcReadOutcome::End) {
                const AttemptResult result{
                        .error = transport_error(common::IoErr::ConnReset, "Nacos setup stream ended")};
                co_await cleanup();
                co_return result;
            }
            auto handled = handle_inbound(generation, inbound, true);
            if (!handled) {
                const AttemptResult result{.error = std::move(handled.error())};
                co_await cleanup();
                co_return result;
            }
            acked = *handled;
        }
    } else {
        generation.stream.clear_local_deadline();
        remaining = remaining_until(handshake_deadline, event::EventLoop::current().now());
        if (remaining < options_->grpc_compatibility_setup_delay) {
            const AttemptResult result{
                    .error = transport_error(common::IoErr::TimedOut, "Nacos compatibility setup timed out")};
            co_await cleanup();
            co_return result;
        }
        auto stop_subscriber = generation.stop_watch.subscribe();
        const std::uint64_t stop_version = stop_subscriber.current().version;
        auto wait = co_await async::timeout_for(
                [&stop_subscriber, stop_version]() { return stop_subscriber.next(stop_version); },
                options_->grpc_compatibility_setup_delay);
        if (wait || generation.stopping) {
            const AttemptResult result{.error = shutdown_error()};
            co_await cleanup();
            co_return result;
        }
    }

    if (generation.stopping) {
        const AttemptResult result{.error = shutdown_error()};
        co_await cleanup();
        co_return result;
    }
    generation.stream.clear_local_deadline();
    generation.reached_ready = true;
    active_requester_ = generation.requester.get();
    ++snapshot_.generation;
    publish_state(NacosGrpcConnectionState::Ready, std::nullopt, state_server_index);

    generation.tasks.add();
    async::spawn(*loop_, [this, &generation]() { return run_heartbeat(&generation); });

    NacosRpcError final_error = transport_error(common::IoErr::ConnReset, "Nacos bidirectional stream ended");
    while (!generation.stopping) {
        proto::Payload inbound;
        auto read = co_await generation.stream.read(inbound);
        if (!read) {
            final_error = transport_error(read.error());
            break;
        }
        if (*read == grpc::GrpcReadOutcome::End) {
            auto status = co_await generation.stream.finish();
            if (!status) {
                final_error = transport_error(status.error());
            } else if (!status->ok()) {
                final_error = NacosRpcError{
                        .code = NacosRpcErrorCode::GrpcStatus,
                        .grpc_status = status->code,
                        .message = status->message.substr(0, 512),
                };
            }
            break;
        }
        auto handled = handle_inbound(generation, inbound, false);
        if (!handled) {
            final_error = std::move(handled.error());
            break;
        }
    }
    if (generation.stopping && control_.stopping) {
        final_error = shutdown_error();
    } else if (generation.stopping && generation.stop_reason != common::IoErr::Canceled) {
        final_error = transport_error(generation.stop_reason);
    }

    const AttemptResult result{
            .error = std::move(final_error),
            .reached_ready = true,
    };
    co_await cleanup();
    co_return result;
}

async::Task<void> NacosGrpcConnection::run() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(!run_active_);
    run_active_ = true;
    auto control_subscriber = control_watch_.subscribe();
    auto retry_delay = options_->grpc_reconnect_initial_delay;

    while (!control_.stopping) {
        if (!auth_ready()) {
            publish_state(NacosGrpcConnectionState::WaitingAuth);
            const auto current = control_subscriber.current();
            auto next = co_await control_subscriber.next(current.version);
            (void) next;
            continue;
        }

        bool attempted = false;
        if (redirect_) {
            Endpoint endpoint = *redirect_;
            redirect_.reset();
            attempted = true;
            auto result = co_await run_generation(endpoint);
            if (result.reached_ready) {
                retry_delay = options_->grpc_reconnect_initial_delay;
            }
            if (control_.stopping) {
                break;
            }
            if (!auth_ready()) {
                continue;
            }
            snapshot_.last_error = std::move(result.error);
        }

        const std::size_t server_count = config_->server_ips().size();
        for (std::size_t offset = 0; offset < server_count && !control_.stopping && auth_ready(); ++offset) {
            const std::size_t server_index = (preferred_server_index_ + offset) % server_count;
            Endpoint endpoint{
                    .ip = config_->server_ips()[server_index],
                    .port = config_->grpc_port(),
                    .server_index = server_index,
            };
            attempted = true;
            auto result = co_await run_generation(endpoint);
            if (result.reached_ready) {
                preferred_server_index_ = server_index;
                retry_delay = options_->grpc_reconnect_initial_delay;
            }
            snapshot_.last_error = result.error;
            if (control_.stopping || !auth_ready() || redirect_) {
                break;
            }
        }

        if (control_.stopping) {
            break;
        }
        if (!auth_ready()) {
            continue;
        }
        if (redirect_) {
            continue;
        }
        FIBER_ASSERT(attempted);
        publish_state(NacosGrpcConnectionState::Backoff, snapshot_.last_error);
        const auto current = control_subscriber.current();
        auto wait = co_await async::timeout_for(
                [&control_subscriber, version = current.version]() { return control_subscriber.next(version); },
                jittered(retry_delay));
        if (!wait && wait.error() == common::IoErr::TimedOut) {
            if (retry_delay < options_->grpc_reconnect_max_delay) {
                retry_delay = retry_delay > options_->grpc_reconnect_max_delay / 2 ? options_->grpc_reconnect_max_delay
                                                                                   : retry_delay * 2;
            }
        }
    }

    publish_state(NacosGrpcConnectionState::Stopping);
    publish_state(NacosGrpcConnectionState::Stopped);
    run_active_ = false;
}

} // namespace fiber::nacos::detail
