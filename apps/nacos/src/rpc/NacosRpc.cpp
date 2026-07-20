#include "NacosRpc.h"

#include <algorithm>
#include <charconv>
#include <utility>

#include <async/Timeout.h>
#include <common/Assert.h>
#include <common/json/JsonValue.h>
#include <net/SocketAddress.h>

#include "../detail/NacosClientImpl.h"

namespace fiber::nacos::detail {
namespace {

constexpr std::chrono::milliseconds kAuthStopPollInterval{100};

NacosRpcError transport_error(common::IoErr error, std::string message = {}) {
    return NacosRpcError{
            .code = NacosRpcErrorCode::Transport,
            .io_error = error,
            .message = std::move(message),
    };
}

NacosRpcError protocol_error(std::string message) {
    return NacosRpcError{
            .code = NacosRpcErrorCode::Protocol,
            .io_error = common::IoErr::Invalid,
            .message = std::move(message),
    };
}

NacosRpcError grpc_status_error(const grpc::GrpcStatus &status) {
    NacosRpcError error{
            .code = NacosRpcErrorCode::GrpcStatus,
            .grpc_status = status.code,
    };
    error.message.assign(status.message.substr(0, 512));
    return error;
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

std::string make_authority(const NacosRpcEndpoint &endpoint) {
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

grpc::GrpcClient::Options make_client_options(const NacosClientOptions &options, const NacosRpcEndpoint &endpoint,
                                              std::string_view authority) {
    grpc::GrpcClient::Options result;
    result.peer_addr = net::SocketAddress(endpoint.ip, endpoint.port);
    result.tcp = options.grpc_tcp;
    result.authority = authority;
    result.scheme = "http";
    return result;
}

NacosRpcCloseKind close_kind_for_error(const NacosRpcError &error) noexcept {
    switch (error.code) {
        case NacosRpcErrorCode::GrpcStatus:
            return NacosRpcCloseKind::GrpcStatusError;
        case NacosRpcErrorCode::Protocol:
            return NacosRpcCloseKind::ProtocolError;
        case NacosRpcErrorCode::Shutdown:
            return NacosRpcCloseKind::Shutdown;
        case NacosRpcErrorCode::InvalidState:
        case NacosRpcErrorCode::AuthenticationUnavailable:
        case NacosRpcErrorCode::Transport:
        case NacosRpcErrorCode::Server:
            return NacosRpcCloseKind::TransportError;
    }
    return NacosRpcCloseKind::TransportError;
}

} // namespace

NacosRpc::NacosRpc(NacosClientImpl &owner, NacosRpcEndpoint endpoint, NacosRpcModule module) :
    NacosRpc(
            NacosRpcDependencies{
                    .loop = owner.loop(),
                    .config = owner.config(),
                    .options = owner.options(),
                    .auth_watch = owner.auth_watch(),
            },
            std::move(endpoint), module) {}

NacosRpc::NacosRpc(NacosRpcDependencies dependencies, NacosRpcEndpoint endpoint, NacosRpcModule module) :
    loop_(&dependencies.loop), config_(&dependencies.config), options_(&dependencies.options),
    auth_subscriber_(dependencies.auth_watch.subscribe()), endpoint_(std::move(endpoint)), module_(module),
    authority_(make_authority(endpoint_)), client_(*loop_, make_client_options(*options_, endpoint_, authority_)) {
    FIBER_ASSERT(endpoint_.port != 0);
    FIBER_ASSERT(!endpoint_.ip.is_unspecified());
    FIBER_ASSERT(!endpoint_.ip.is_multicast());
    stop_publisher_ = stop_watch_.acquire_publisher();
    FIBER_ASSERT(stop_publisher_.has_value());
    state_publisher_ = state_watch_.acquire_publisher();
    FIBER_ASSERT(state_publisher_.has_value());
}

NacosRpc::~NacosRpc() {
    FIBER_ASSERT(state_ == NacosRpcState::Created || state_ == NacosRpcState::Stopped);
    FIBER_ASSERT(operations_.empty());
}

NacosRpcError NacosRpc::invalid_state_error(std::string message) {
    return NacosRpcError{
            .code = NacosRpcErrorCode::InvalidState,
            .io_error = common::IoErr::Already,
            .message = std::move(message),
    };
}

NacosRpcError NacosRpc::shutdown_error() {
    return NacosRpcError{
            .code = NacosRpcErrorCode::Shutdown,
            .io_error = common::IoErr::Canceled,
    };
}

std::expected<NacosRpc::MetadataSnapshot, NacosRpcError> NacosRpc::current_metadata() const noexcept {
    if (client_ip_.empty()) {
        return std::unexpected(protocol_error("Nacos gRPC local address is unavailable"));
    }

    MetadataSnapshot result;
    auto auth = auth_subscriber_.current();
    result.auth = std::move(auth.value);
    if (!result.auth) {
        return std::unexpected(NacosRpcError{
                .code = NacosRpcErrorCode::AuthenticationUnavailable,
                .io_error = common::IoErr::Permission,
        });
    }

    std::optional<std::string_view> access_token;
    switch (result.auth->kind) {
        case NacosAuthAccessKind::NotConfigured:
            FIBER_ASSERT(result.auth->access_token.empty());
            break;
        case NacosAuthAccessKind::Present:
            FIBER_ASSERT(!result.auth->access_token.empty());
            access_token = result.auth->access_token;
            break;
        case NacosAuthAccessKind::InitialFailed:
            return std::unexpected(NacosRpcError{
                    .code = NacosRpcErrorCode::AuthenticationUnavailable,
                    .io_error = common::IoErr::Permission,
            });
        case NacosAuthAccessKind::Stopped:
            return std::unexpected(shutdown_error());
    }

    result.metadata = NacosPayloadMetadata{
            .client_ip = client_ip_,
            .client_version = config_->client_version(),
            .namespace_id = config_->namespace_id(),
            .access_token = access_token,
    };
    return result;
}

async::Task<std::expected<proto::Payload, NacosRpcError>>
NacosRpc::request_payload(const proto::Payload &request, mem::BufPool &pool,
                          std::chrono::milliseconds timeout) noexcept {
    grpc::GrpcStream stream =
            client_.open_stream("Request", "request", pool,
                                {
                                        .deadline = timeout,
                                        .max_inbound_message_bytes = options_->max_inbound_grpc_message_bytes,
                                });
    auto result = co_await stream.open();
    if (!result) {
        co_return std::unexpected(transport_error(result.error()));
    }
    result = co_await stream.write(request);
    if (!result) {
        co_return std::unexpected(transport_error(result.error()));
    }
    result = co_await stream.writes_done();
    if (!result) {
        co_return std::unexpected(transport_error(result.error()));
    }

    proto::Payload response;
    auto read = co_await stream.read(response);
    if (!read) {
        co_return std::unexpected(transport_error(read.error()));
    }
    if (*read != grpc::GrpcReadOutcome::Message) {
        co_return std::unexpected(protocol_error("Nacos unary RPC returned no response"));
    }

    auto extra = co_await stream.read(response);
    if (!extra) {
        co_return std::unexpected(transport_error(extra.error()));
    }
    if (*extra == grpc::GrpcReadOutcome::Message) {
        co_return std::unexpected(protocol_error("Nacos unary RPC returned multiple responses"));
    }

    auto status = co_await stream.finish();
    if (!status) {
        co_return std::unexpected(transport_error(status.error()));
    }
    if (!status->ok()) {
        co_return std::unexpected(grpc_status_error(*status));
    }
    co_return response;
}

async::Task<NacosRpcCloseResult> NacosRpc::finish_run() noexcept {
    if (client_connected_) {
        co_await client_.shutdown();
    }
    co_await operations_.join();
    stream_ = grpc::GrpcStream{};
    mark_stopped();
    co_return close_result_;
}

async::Task<NacosRpcCloseResult> NacosRpc::finish_run_error(NacosRpcError error) noexcept {
    begin_stop(close_kind_for_error(error), error);
    co_return co_await finish_run();
}

async::Task<NacosRpcCloseResult> NacosRpc::run(const NacosBiRequestHandler &handlers) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(!run_started_);
    run_started_ = true;
    if (stop_requested_) {
        mark_stopped();
        co_return close_result_;
    }
    FIBER_ASSERT(state_ == NacosRpcState::Created);

    set_state(NacosRpcState::WaitingAuth);

    auto auth = auth_subscriber_.current();
    std::uint64_t auth_version = auth.version;
    while (!stop_requested_ && (!auth.value || auth.value->kind == NacosAuthAccessKind::InitialFailed)) {
        auto next = co_await async::timeout_for([&]() { return auth_subscriber_.next(auth_version); },
                                                kAuthStopPollInterval);
        if (next) {
            auth = std::move(*next);
            auth_version = auth.version;
        } else if (next.error() != common::IoErr::TimedOut) {
            co_return co_await finish_run_error(transport_error(next.error()));
        }
    }
    if (stop_requested_ || (auth.value && auth.value->kind == NacosAuthAccessKind::Stopped)) {
        co_return co_await finish_run_error(shutdown_error());
    }
    FIBER_ASSERT(auth.value != nullptr);

    set_state(NacosRpcState::Connecting);
    auto connected = co_await client_.connect(options_->grpc_connect_timeout);
    if (!connected) {
        co_return co_await finish_run_error(transport_error(connected.error()));
    }
    client_connected_ = true;
    if (stop_requested_) {
        co_return co_await finish_run_error(shutdown_error());
    }

    if (!options_->client_ip_override.empty()) {
        client_ip_ = options_->client_ip_override;
    } else if (client_.local_addr()) {
        client_ip_ = client_.local_addr()->ip().to_string();
    }
    if (client_ip_.empty()) {
        co_return co_await finish_run_error(protocol_error("Nacos gRPC local address is unavailable"));
    }

    const auto handshake_deadline = event::EventLoop::current().now() + options_->grpc_handshake_timeout;
    auto request_timeout = std::min(options_->grpc_request_timeout,
                                    remaining_until(handshake_deadline, event::EventLoop::current().now()));
    if (request_timeout <= std::chrono::milliseconds::zero()) {
        co_return co_await finish_run_error(transport_error(common::IoErr::TimedOut, "Nacos handshake timed out"));
    }

    set_state(NacosRpcState::Checking);
    auto metadata = current_metadata();
    if (!metadata) {
        co_return co_await finish_run_error(std::move(metadata.error()));
    }
    dto::req::ServerCheckRequest check_request;
    auto check_payload = encode_payload(check_request, metadata->metadata, options_->max_inbound_grpc_message_bytes);
    if (!check_payload) {
        co_return co_await finish_run_error(std::move(check_payload.error()));
    }
    mem::BufPool check_pool;
    auto check_response_payload = co_await request_payload(*check_payload, check_pool, request_timeout);
    if (!check_response_payload) {
        co_return co_await finish_run_error(std::move(check_response_payload.error()));
    }
    dto::resp::ServerCheckResponse check_response;
    auto decoded = decode_payload(*check_response_payload, options_->max_inbound_grpc_message_bytes, check_pool,
                                  check_response);
    if (!decoded) {
        co_return co_await finish_run_error(std::move(decoded.error()));
    }
    if (!check_response.success()) {
        NacosRpcError error{
                .code = NacosRpcErrorCode::Server,
                .result_code = check_response.result_code,
                .error_code = check_response.error_code,
        };
        if (check_response.message.is_present()) {
            error.message.assign(check_response.message.value().substr(0, 512));
        }
        co_return co_await finish_run_error(std::move(error));
    }
    if (check_response.connection_id.is_present()) {
        connection_id_.assign(check_response.connection_id.value());
    }
    support_ability_negotiation_ = check_response.support_ability_negotiation;

    set_state(NacosRpcState::Handshaking);
    stream_ = client_.open_stream("BiRequestStream", "requestBiStream", stream_pool_,
                                  {.max_inbound_message_bytes = options_->max_inbound_grpc_message_bytes});
    auto remaining = remaining_until(handshake_deadline, event::EventLoop::current().now());
    if (remaining <= std::chrono::milliseconds::zero()) {
        co_return co_await finish_run_error(transport_error(common::IoErr::TimedOut, "Nacos handshake timed out"));
    }
    stream_.set_local_deadline(remaining);
    auto stream_result = co_await stream_.open();
    if (!stream_result) {
        co_return co_await finish_run_error(transport_error(stream_result.error()));
    }

    json::JsonObject<std::string_view>::Entry label_entries[] = {
            {.key = "source", .value = "sdk"},
            {.key = "module", .value = nacos_rpc_module_name(module_)},
    };
    dto::req::ConnectionSetupRequest setup;
    setup.client_version.set_present(config_->client_version());
    setup.tenant.set_present(config_->tenant());
    setup.labels.set_present(json::JsonObject<std::string_view>(label_entries, std::size(label_entries)));
    setup.ability_table.set_present(json::JsonObject<bool>());
    metadata = current_metadata();
    if (!metadata) {
        co_return co_await finish_run_error(std::move(metadata.error()));
    }
    auto setup_payload = encode_payload(setup, metadata->metadata, options_->max_inbound_grpc_message_bytes);
    if (!setup_payload) {
        co_return co_await finish_run_error(std::move(setup_payload.error()));
    }
    stream_result = co_await stream_.write(*setup_payload);
    if (!stream_result) {
        co_return co_await finish_run_error(transport_error(stream_result.error()));
    }

    if (support_ability_negotiation_) {
        bool setup_acked = false;
        while (!stop_requested_ && !setup_acked) {
            proto::Payload inbound;
            auto read = co_await stream_.read(inbound);
            if (!read) {
                co_return co_await finish_run_error(transport_error(read.error()));
            }
            if (*read == grpc::GrpcReadOutcome::End) {
                co_return co_await finish_run_error(
                        transport_error(common::IoErr::ConnReset, "Nacos setup stream ended"));
            }
            auto action = co_await dispatch_inbound(handlers, inbound);
            if (!action) {
                co_return co_await finish_run_error(std::move(action.error()));
            }
            if (action->has_response) {
                stream_result = co_await stream_.write(action->response);
                if (!stream_result) {
                    co_return co_await finish_run_error(transport_error(stream_result.error()));
                }
            }
            setup_acked = action->setup_ack;
            if (action->close_after_response) {
                NacosRpcError error = transport_error(common::IoErr::ConnReset, "Nacos server redirected connection");
                begin_stop(NacosRpcCloseKind::Redirect, error, std::move(action->redirect));
                co_return co_await finish_run();
            }
        }
    } else {
        stream_.clear_local_deadline();
        remaining = remaining_until(handshake_deadline, event::EventLoop::current().now());
        if (remaining < options_->grpc_compatibility_setup_delay) {
            co_return co_await finish_run_error(
                    transport_error(common::IoErr::TimedOut, "Nacos compatibility setup timed out"));
        }
        auto stopped = stop_watch_.subscribe();
        const auto stop_version = stopped.current().version;
        auto wait = co_await async::timeout_for([&]() { return stopped.next(stop_version); },
                                                options_->grpc_compatibility_setup_delay);
        if (wait || stop_requested_) {
            co_return co_await finish_run_error(shutdown_error());
        }
        if (wait.error() != common::IoErr::TimedOut) {
            co_return co_await finish_run_error(transport_error(wait.error()));
        }
    }

    if (stop_requested_) {
        co_return co_await finish_run_error(shutdown_error());
    }
    stream_.clear_local_deadline();
    set_state(NacosRpcState::Ready);

    operations_.add();
    async::spawn(*loop_, [this]() { return run_heartbeat(); });
    co_await run_server_requests(handlers);
    co_return co_await finish_run();
}

void NacosRpc::save_redirect(const dto::req::ConnectResetRequest &request, InboundAction &action) const noexcept {
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
        std::string_view text = request.server_port.value();
        auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size() || parsed == 0 || parsed > 65535) {
            return;
        }
        port = static_cast<std::uint16_t>(parsed);
    }
    action.redirect = NacosRpcEndpoint{
            .ip = ip,
            .port = port,
            .server_index = std::nullopt,
    };
}

async::Task<std::expected<NacosRpc::InboundAction, NacosRpcError>>
NacosRpc::dispatch_inbound(const NacosBiRequestHandler &handlers, const proto::Payload &payload) noexcept {
    auto view = validate_payload(payload, options_->max_inbound_grpc_message_bytes);
    if (!view) {
        co_return std::unexpected(std::move(view.error()));
    }

    InboundAction action;
    if (view->type == dto::req::SetupAckRequest::kTypeName) {
        mem::BufPool pool;
        dto::req::SetupAckRequest request;
        auto parsed = parse_payload_json(*view, pool, request);
        if (!parsed) {
            co_return std::unexpected(std::move(parsed.error()));
        }
        action.setup_ack = true;
        co_return action;
    }

    if (!ends_with_request(view->type)) {
        co_return action;
    }

    auto metadata = current_metadata();
    if (!metadata) {
        co_return std::unexpected(std::move(metadata.error()));
    }

    if (view->type == dto::req::ClientDetectionRequest::kTypeName) {
        mem::BufPool pool;
        dto::req::ClientDetectionRequest request;
        auto parsed = parse_payload_json(*view, pool, request);
        if (!parsed) {
            co_return std::unexpected(std::move(parsed.error()));
        }
        dto::resp::ClientDetectionResponse response;
        response.request_id = request.request_id;
        auto encoded = encode_payload(response, metadata->metadata, options_->max_push_response_bytes);
        if (!encoded) {
            co_return std::unexpected(std::move(encoded.error()));
        }
        action.response = std::move(*encoded);
        action.has_response = true;
        co_return action;
    }

    if (view->type == dto::req::ConnectResetRequest::kTypeName) {
        mem::BufPool pool;
        dto::req::ConnectResetRequest request;
        auto parsed = parse_payload_json(*view, pool, request);
        if (!parsed) {
            co_return std::unexpected(std::move(parsed.error()));
        }
        dto::resp::ConnectResetResponse response;
        response.request_id = request.request_id;
        auto encoded = encode_payload(response, metadata->metadata, options_->max_push_response_bytes);
        if (!encoded) {
            co_return std::unexpected(std::move(encoded.error()));
        }
        action.response = std::move(*encoded);
        action.has_response = true;
        action.close_after_response = true;
        save_redirect(request, action);
        co_return action;
    }

    auto handled = co_await handlers.dispatch(nacos_rpc_module_name(module_), *view, payload.metadata(),
                                              metadata->metadata, options_->max_push_response_bytes);
    if (!handled) {
        co_return std::unexpected(std::move(handled.error()));
    }
    if (*handled) {
        action.response = std::move(**handled);
        action.has_response = true;
        co_return action;
    }

    mem::BufPool pool;
    dto::RequestBase request;
    auto parsed = parse_payload_json(*view, pool, request);
    if (!parsed) {
        co_return std::unexpected(std::move(parsed.error()));
    }
    dto::resp::ErrorResponse response;
    response.result_code = dto::kResponseFail;
    response.error_code = dto::kResponseFail;
    response.message.set_present("unsupported Nacos server request");
    response.request_id = request.request_id;
    auto encoded = encode_payload(response, metadata->metadata, options_->max_push_response_bytes);
    if (!encoded) {
        co_return std::unexpected(std::move(encoded.error()));
    }
    action.response = std::move(*encoded);
    action.has_response = true;
    co_return action;
}

async::Task<void> NacosRpc::run_server_requests(const NacosBiRequestHandler &handlers) noexcept {
    while (!stop_requested_) {
        proto::Payload inbound;
        auto read = co_await stream_.read(inbound);
        if (!read) {
            if (!stop_requested_) {
                NacosRpcError error = transport_error(read.error());
                begin_stop(NacosRpcCloseKind::TransportError, error);
            }
            break;
        }
        if (*read == grpc::GrpcReadOutcome::End) {
            auto status = co_await stream_.finish();
            if (!status) {
                NacosRpcError error = transport_error(status.error());
                begin_stop(NacosRpcCloseKind::TransportError, error);
            } else if (!status->ok()) {
                NacosRpcError error = grpc_status_error(*status);
                begin_stop(NacosRpcCloseKind::GrpcStatusError, error);
            } else {
                NacosRpcError error = transport_error(common::IoErr::ConnReset, "Nacos bidirectional stream ended");
                begin_stop(NacosRpcCloseKind::PeerClosed, error);
            }
            break;
        }

        auto action = co_await dispatch_inbound(handlers, inbound);
        if (!action) {
            NacosRpcError error = std::move(action.error());
            begin_stop(close_kind_for_error(error), error);
            break;
        }
        if (action->has_response) {
            auto written = co_await stream_.write(action->response);
            if (!written) {
                NacosRpcError error = transport_error(written.error());
                begin_stop(NacosRpcCloseKind::TransportError, error);
                break;
            }
        }
        if (action->close_after_response) {
            NacosRpcError error = transport_error(common::IoErr::ConnReset, "Nacos server redirected connection");
            begin_stop(NacosRpcCloseKind::Redirect, error, std::move(action->redirect));
            break;
        }
    }

    co_return;
}

async::DetachedTask NacosRpc::run_heartbeat() noexcept {
    TaskDoneGuard done(operations_);
    auto stopped = stop_watch_.subscribe();
    std::uint64_t stop_version = stopped.current().version;
    while (!stop_requested_) {
        auto wait = co_await async::timeout_for([&]() { return stopped.next(stop_version); },
                                                options_->grpc_heartbeat_interval);
        if (wait) {
            stop_version = wait->version;
            break;
        }
        if (wait.error() != common::IoErr::TimedOut || stop_requested_) {
            break;
        }

        dto::req::HealthCheckRequest request_value;
        dto::resp::HealthCheckResponse response;
        mem::BufPool pool;
        auto result = co_await request(request_value, pool, response);
        if (!result && result.error().code == NacosRpcErrorCode::Shutdown) {
            break;
        }
    }
}

void NacosRpc::begin_stop(NacosRpcCloseKind kind, const NacosRpcError &error,
                          std::optional<NacosRpcEndpoint> redirect) noexcept {
    if (close_result_.kind == NacosRpcCloseKind::None) {
        close_result_ = NacosRpcCloseResult{
                .kind = kind,
                .error = error,
                .redirect = std::move(redirect),
        };
    }
    if (stop_requested_) {
        return;
    }
    stop_requested_ = true;
    if (state_ != NacosRpcState::Stopped) {
        set_state(NacosRpcState::Stopping);
    }
    if (stream_.valid()) {
        stream_.cancel(error.io_error == common::IoErr::None ? common::IoErr::Canceled : error.io_error);
    }
    stop_publisher_->publish(true);
}

void NacosRpc::set_state(NacosRpcState state) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    state_publisher_->publish(state);
}

void NacosRpc::mark_stopped() noexcept {
    if (state_ == NacosRpcState::Stopped) {
        return;
    }
    set_state(NacosRpcState::Stopped);
}

async::Task<std::expected<void, NacosRpcError>> NacosRpc::wait_ready() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    auto states = state_watch_.subscribe();
    auto snapshot = states.current();
    for (;;) {
        FIBER_ASSERT(snapshot.value != nullptr);
        switch (*snapshot.value) {
            case NacosRpcState::Ready:
                co_return std::expected<void, NacosRpcError>{};
            case NacosRpcState::Stopping:
            case NacosRpcState::Stopped:
                co_return std::unexpected(close_result_.error);
            case NacosRpcState::Created:
            case NacosRpcState::WaitingAuth:
            case NacosRpcState::Connecting:
            case NacosRpcState::Checking:
            case NacosRpcState::Handshaking:
                snapshot = co_await states.next(snapshot.version);
                break;
        }
    }
}

void NacosRpc::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == NacosRpcState::Stopped || stop_requested_) {
        return;
    }
    NacosRpcError error = shutdown_error();
    begin_stop(NacosRpcCloseKind::Shutdown, error);
    if (!run_started_) {
        mark_stopped();
    }
}

} // namespace fiber::nacos::detail
