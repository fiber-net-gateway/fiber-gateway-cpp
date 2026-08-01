#ifndef FIBER_NACOS_RPC_NACOS_RPC_H
#define FIBER_NACOS_RPC_NACOS_RPC_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <async/Spawn.h>
#include <async/Task.h>
#include <async/WaitGroup.h>
#include <async/Watch.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <fiber/nacos/NacosAuthAccess.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <fiber/nacos/NacosRpcOptions.h>
#include <grpc/GrpcClient.h>
#include <grpc/GrpcStream.h>
#include <nacos_grpc_payload.pb.h>
#include <net/IpAddress.h>
#include "../dto/Internal.h"

#include "NacosBiRequestHandler.h"

namespace fiber::nacos::detail {

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
    const NacosRpcOptions &options;
    const NacosAuthSubscriber &auth;
};

// One independent Nacos gRPC connection. This class intentionally does not
// reconnect or publish service-level state: its owner chooses an endpoint,
// constructs a new instance, and owns the run() task through completion. Every
// method and registered handler runs on the construction EventLoop.
class NacosRpc : public common::NonCopyable, public common::NonMovable {
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
    NacosRpc(NacosRpcDependencies dependencies, NacosRpcEndpoint endpoint, NacosRpcModule module);
    ~NacosRpc();

    [[nodiscard]] static bool valid_options(const NacosRpcOptions &options) noexcept;

    // Runs exactly one connection from authentication wait through complete
    // gRPC teardown. handlers and all registered callback contexts must outlive
    // this task and must not be modified before it returns.
    [[nodiscard]] async::Task<NacosRpcCloseResult> run(const NacosBiRequestHandler &handlers) noexcept;
    [[nodiscard]] async::Task<std::expected<void, NacosRpcError>> wait_ready() noexcept;

    // Requests stop without waiting. run() remains the completion barrier and
    // performs all asynchronous transport teardown before returning.
    void shutdown() noexcept;

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
        auto payload = encode_payload(request, metadata->metadata, options_->max_inbound_message_bytes);
        if (!payload) {
            co_return std::unexpected(std::move(payload.error()));
        }
        mem::BufPool transport_pool;
        auto response_payload = co_await request_payload(*payload, transport_pool, options_->request_timeout);
        if (!response_payload) {
            co_return std::unexpected(std::move(response_payload.error()));
        }
        co_return decode_payload(*response_payload, options_->max_inbound_message_bytes, pool, response);
    }

    [[nodiscard]] NacosRpcState state() const noexcept { return state_; }
    [[nodiscard]] const NacosRpcEndpoint &endpoint() const noexcept { return endpoint_; }
    [[nodiscard]] NacosRpcModule module() const noexcept { return module_; }
    [[nodiscard]] const std::string &connection_id() const noexcept { return connection_id_; }
    [[nodiscard]] bool support_ability_negotiation() const noexcept { return support_ability_negotiation_; }
    [[nodiscard]] const NacosRpcCloseResult &close_result() const noexcept { return close_result_; }

private:
    [[nodiscard]] async::Task<std::expected<proto::Payload, NacosRpcError>>
    request_payload(const proto::Payload &request, mem::BufPool &pool, std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] async::Task<NacosRpcCloseResult> finish_run() noexcept;
    [[nodiscard]] async::Task<NacosRpcCloseResult> finish_run_error(NacosRpcError error) noexcept;
    [[nodiscard]] async::Task<void> run_server_requests(const NacosBiRequestHandler &handlers) noexcept;
    [[nodiscard]] async::DetachedTask run_heartbeat() noexcept;
    [[nodiscard]] async::Task<std::expected<InboundAction, NacosRpcError>>
    dispatch_inbound(const NacosBiRequestHandler &handlers, const proto::Payload &payload) noexcept;
    [[nodiscard]] std::expected<MetadataSnapshot, NacosRpcError> current_metadata() const noexcept;
    void begin_stop(NacosRpcCloseKind kind, const NacosRpcError &error,
                    std::optional<NacosRpcEndpoint> redirect = std::nullopt) noexcept;
    void save_redirect(const dto::req::ConnectResetRequest &request, InboundAction &action) const noexcept;
    void set_state(NacosRpcState state);
    void mark_stopped() noexcept;
    [[nodiscard]] static NacosRpcError invalid_state_error(std::string message);
    [[nodiscard]] static NacosRpcError shutdown_error();

    event::EventLoop *loop_ = nullptr;
    const NacosClientConfig *config_ = nullptr;
    const NacosRpcOptions *options_ = nullptr;
    const NacosAuthSubscriber *auth_subscriber_ = nullptr;
    NacosRpcEndpoint endpoint_;
    NacosRpcModule module_ = NacosRpcModule::Config;
    std::string authority_;
    grpc::GrpcClient client_;
    mem::BufPool stream_pool_;
    grpc::GrpcStream stream_;
    async::WaitGroup operations_;
    async::Watch<bool> stop_watch_{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher_;
    async::Watch<NacosRpcState> state_watch_{NacosRpcState::Created};
    std::optional<async::Watch<NacosRpcState>::Publisher> state_publisher_;
    NacosRpcState state_ = NacosRpcState::Created;
    NacosRpcCloseResult close_result_;
    std::string client_ip_;
    std::string connection_id_;
    bool support_ability_negotiation_ = false;
    bool run_started_ = false;
    bool stop_requested_ = false;
    bool client_connected_ = false;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_RPC_NACOS_RPC_H
