#ifndef FIBER_NACOS_RPC_NACOS_RPC_H
#define FIBER_NACOS_RPC_NACOS_RPC_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include <async/Spawn.h>
#include <async/Task.h>
#include <async/WaitGroup.h>
#include <async/Watch.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <fiber/nacos/NacosAuthAccess.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <fiber/nacos/dto/Internal.h>
#include <grpc/GrpcClient.h>
#include <grpc/GrpcStream.h>
#include <nacos_grpc_payload.pb.h>
#include <net/IpAddress.h>

#include "NacosPayloadCodec.h"
#include "NacosRequestHandler.h"

namespace fiber::nacos::detail {

class NacosClientImpl;

enum class NacosRpcModule : std::uint8_t {
    Config,
    Naming,
};

[[nodiscard]] constexpr std::string_view nacos_rpc_module_name(NacosRpcModule module) noexcept {
    switch (module) {
        case NacosRpcModule::Config:
            return "config";
        case NacosRpcModule::Naming:
            return "naming";
    }
    return {};
}

struct NacosRpcEndpoint {
    net::IpAddress ip;
    std::uint16_t port = 0;
    std::optional<std::size_t> server_index;
};

enum class NacosRpcState : std::uint8_t {
    Created,
    WaitingAuth,
    Connecting,
    Checking,
    Handshaking,
    Ready,
    Stopping,
    Stopped,
};

enum class NacosRpcCloseKind : std::uint8_t {
    None,
    PeerClosed,
    TransportError,
    GrpcStatusError,
    ProtocolError,
    Redirect,
    Shutdown,
};

struct NacosRpcCloseResult {
    NacosRpcCloseKind kind = NacosRpcCloseKind::None;
    NacosRpcError error;
    std::optional<NacosRpcEndpoint> redirect;
};

struct NacosRpcDependencies {
    event::EventLoop &loop;
    const NacosClientConfig &config;
    const NacosClientOptions &options;
    async::Watch<NacosAuthAccess>::Subscriber auth_subscriber;
};

// One independent Nacos gRPC connection. This class intentionally does not
// reconnect or publish service-level state: its owner chooses an endpoint,
// constructs a new instance, and must await shutdown() before destruction.
// Every method and registered handler runs on the construction EventLoop.
class NacosRpc : public common::NonCopyable, public common::NonMovable {
    struct HandlerEntry;

    using ErasedHandler = void (*)() noexcept;
    using InvokeHandler = std::expected<proto::Payload, NacosRpcError> (*)(
            const HandlerEntry &entry, NacosRpc &rpc, const NacosPayloadView &payload, const proto::Metadata &metadata,
            const NacosPayloadMetadata &outbound_metadata) noexcept;

    struct HandlerEntry {
        std::string_view type;
        void *context = nullptr;
        ErasedHandler handler = nullptr;
        InvokeHandler invoke = nullptr;
    };

    struct MetadataSnapshot {
        std::shared_ptr<const NacosAuthAccess> auth;
        NacosPayloadMetadata metadata;
    };

    struct InboundAction {
        proto::Payload response;
        std::optional<NacosRpcEndpoint> redirect;
        bool has_response = false;
        bool setup_ack = false;
        bool close_after_response = false;
    };

    class TaskDoneGuard {
    public:
        explicit TaskDoneGuard(async::WaitGroup &group) noexcept : group_(&group) {}
        TaskDoneGuard(const TaskDoneGuard &) = delete;
        TaskDoneGuard &operator=(const TaskDoneGuard &) = delete;
        ~TaskDoneGuard() { group_->done(); }

    private:
        async::WaitGroup *group_ = nullptr;
    };

public:
    static constexpr std::size_t kMaxRequestHandlers = 16;

    NacosRpc(NacosClientImpl &owner, NacosRpcEndpoint endpoint, NacosRpcModule module);
    NacosRpc(NacosRpcDependencies dependencies, NacosRpcEndpoint endpoint, NacosRpcModule module);
    ~NacosRpc();

    // Registration is allocation-free and is frozen by the first start(). A
    // handler is synchronous/noexcept; it must not retain request/context views.
    template<typename Request>
        requires requires { typename NacosServerRequestTraits<Request>::Response; }
    [[nodiscard]] std::expected<void, NacosHandlerRegistrationError>
    add_request_handler(RequestHandler<Request> handler, void *context) noexcept {
        if (state_ != NacosRpcState::Created || handlers_frozen_) {
            return std::unexpected(NacosHandlerRegistrationError::Started);
        }
        if (!handler) {
            return std::unexpected(NacosHandlerRegistrationError::InvalidHandler);
        }
        static_assert(std::is_base_of_v<dto::RequestBase, Request>);
        static_assert(std::is_base_of_v<dto::ResponseBase, NacosServerResponse<Request>>);
        for (std::size_t i = 0; i < handler_count_; ++i) {
            if (handlers_[i].type == Request::kTypeName) {
                return std::unexpected(NacosHandlerRegistrationError::DuplicateType);
            }
        }
        if (handler_count_ == handlers_.size()) {
            return std::unexpected(NacosHandlerRegistrationError::RegistryFull);
        }
        handlers_[handler_count_++] = HandlerEntry{
                .type = Request::kTypeName,
                .context = context,
                .handler = reinterpret_cast<ErasedHandler>(handler),
                .invoke = &invoke_registered_handler<Request>,
        };
        return {};
    }

    [[nodiscard]] async::Task<std::expected<void, NacosRpcError>> start() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    [[nodiscard]] async::Task<NacosRpcCloseResult> wait_closed() noexcept;

    // Opens one unary /Request/request call on the owned HTTP/2 connection.
    // Authentication metadata is snapshotted for every invocation, so token
    // refresh does not require reconnecting an otherwise healthy NacosRpc.
    template<typename Request, typename Response>
    [[nodiscard]] async::Task<std::expected<void, NacosRpcError>> request(const Request &request, mem::BufPool &pool,
                                                                          Response &response) noexcept {
        FIBER_ASSERT(loop_->in_loop());
        if (state_ != NacosRpcState::Ready || stop_requested_) {
            co_return std::unexpected(state_ == NacosRpcState::Stopping || state_ == NacosRpcState::Stopped
                                              ? shutdown_error()
                                              : invalid_state_error("Nacos RPC is not ready"));
        }

        operations_.add();
        TaskDoneGuard done(operations_);
        auto metadata = current_metadata();
        if (!metadata) {
            co_return std::unexpected(std::move(metadata.error()));
        }
        auto payload = encode_payload(request, metadata->metadata, options_->max_inbound_grpc_message_bytes);
        if (!payload) {
            co_return std::unexpected(std::move(payload.error()));
        }
        auto response_payload = co_await request_payload(*payload, pool, options_->grpc_request_timeout);
        if (!response_payload) {
            co_return std::unexpected(std::move(response_payload.error()));
        }
        co_return decode_payload(*response_payload, options_->max_inbound_grpc_message_bytes, pool, response);
    }

    [[nodiscard]] NacosRpcState state() const noexcept { return state_; }
    [[nodiscard]] const NacosRpcEndpoint &endpoint() const noexcept { return endpoint_; }
    [[nodiscard]] NacosRpcModule module() const noexcept { return module_; }
    [[nodiscard]] const std::string &connection_id() const noexcept { return connection_id_; }
    [[nodiscard]] bool support_ability_negotiation() const noexcept { return support_ability_negotiation_; }
    [[nodiscard]] const NacosRpcCloseResult &close_result() const noexcept { return close_result_; }

private:
    template<typename Request>
    static std::expected<proto::Payload, NacosRpcError>
    invoke_registered_handler(const HandlerEntry &entry, NacosRpc &rpc, const NacosPayloadView &payload,
                              const proto::Metadata &metadata, const NacosPayloadMetadata &outbound_metadata) noexcept {
        auto handler = reinterpret_cast<RequestHandler<Request>>(entry.handler);
        FIBER_ASSERT(handler != nullptr);

        mem::BufPool pool;
        Request request;
        auto parsed = parse_payload_json(payload, pool, request);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }

        NacosServerResponse<Request> response;
        NacosServerRequestContext context(metadata, nacos_rpc_module_name(rpc.module_), pool);
        auto handled = handler(entry.context, context, request, response);
        if (!handled) {
            dto::resp::ErrorResponse error_response;
            error_response.result_code = handled.error().result_code;
            error_response.error_code = handled.error().error_code;
            if (!handled.error().message.empty()) {
                error_response.message.set_present(std::string_view(handled.error().message).substr(0, 512));
            }
            error_response.request_id = request.request_id;
            return encode_payload(error_response, outbound_metadata, rpc.options_->max_inbound_grpc_message_bytes);
        }

        response.request_id = request.request_id;
        return encode_payload(response, outbound_metadata, rpc.options_->max_inbound_grpc_message_bytes);
    }

    [[nodiscard]] async::Task<std::expected<proto::Payload, NacosRpcError>>
    request_payload(const proto::Payload &request, mem::BufPool &pool, std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] async::Task<void> shutdown_client() noexcept;
    [[nodiscard]] async::Task<std::expected<void, NacosRpcError>> finish_start_error(NacosRpcError error) noexcept;
    [[nodiscard]] async::DetachedTask run_server_requests() noexcept;
    [[nodiscard]] async::DetachedTask run_heartbeat() noexcept;
    [[nodiscard]] std::expected<InboundAction, NacosRpcError> dispatch_inbound(const proto::Payload &payload) noexcept;
    [[nodiscard]] std::expected<MetadataSnapshot, NacosRpcError> current_metadata() const noexcept;
    [[nodiscard]] const HandlerEntry *find_handler(std::string_view type) const noexcept;
    void begin_stop(NacosRpcCloseKind kind, const NacosRpcError &error,
                    std::optional<NacosRpcEndpoint> redirect = std::nullopt) noexcept;
    void save_redirect(const dto::req::ConnectResetRequest &request, InboundAction &action) const noexcept;
    void mark_stopped() noexcept;
    [[nodiscard]] static NacosRpcError invalid_state_error(std::string message);
    [[nodiscard]] static NacosRpcError shutdown_error();

    event::EventLoop *loop_ = nullptr;
    const NacosClientConfig *config_ = nullptr;
    const NacosClientOptions *options_ = nullptr;
    async::Watch<NacosAuthAccess>::Subscriber auth_subscriber_;
    NacosRpcEndpoint endpoint_;
    NacosRpcModule module_ = NacosRpcModule::Config;
    std::string authority_;
    grpc::GrpcClient client_;
    mem::BufPool stream_pool_;
    grpc::GrpcStream stream_;
    async::WaitGroup operations_;
    async::Watch<bool> stop_watch_{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher_;
    async::Watch<bool> closed_watch_{false};
    std::optional<async::Watch<bool>::Publisher> closed_publisher_;
    std::array<HandlerEntry, kMaxRequestHandlers> handlers_{};
    std::size_t handler_count_ = 0;
    NacosRpcState state_ = NacosRpcState::Created;
    NacosRpcCloseResult close_result_;
    std::string client_ip_;
    std::string connection_id_;
    bool support_ability_negotiation_ = false;
    bool handlers_frozen_ = false;
    bool stop_requested_ = false;
    bool shutdown_called_ = false;
    bool client_connected_ = false;
    bool client_shutdown_started_ = false;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_RPC_NACOS_RPC_H
