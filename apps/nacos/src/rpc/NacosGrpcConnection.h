#ifndef FIBER_NACOS_RPC_NACOS_GRPC_CONNECTION_H
#define FIBER_NACOS_RPC_NACOS_GRPC_CONNECTION_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <async/Spawn.h>
#include <async/Task.h>
#include <async/Watch.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <fiber/nacos/NacosAuth.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <nacos_grpc_payload.pb.h>

#include "NacosPayloadCodec.h"
#include "NacosRequester.h"

namespace fiber::nacos::detail {

enum class NacosGrpcConnectionState : std::uint8_t {
    Created,
    WaitingAuth,
    Connecting,
    Checking,
    Handshaking,
    Ready,
    Backoff,
    Stopping,
    Stopped,
};

struct NacosGrpcConnectionSnapshot {
    NacosGrpcConnectionState state = NacosGrpcConnectionState::Created;
    std::uint64_t generation = 0;
    std::size_t server_index = 0;
    NacosRpcError last_error;
};

struct NacosPushHandler {
    using Callback = std::expected<proto::Payload, NacosRpcError> (*)(void *context, const proto::Payload &request,
                                                                      const NacosPayloadMetadata &metadata,
                                                                      std::size_t max_payload_bytes) noexcept;

    void *context = nullptr;
    Callback callback = nullptr;
};

class NacosGrpcConnection : public common::NonCopyable, public common::NonMovable {
    struct Generation;

    struct RequestLease {
        Generation *generation = nullptr;
        NacosRequester *requester = nullptr;
    };

public:
    using StateSubscriber = async::Watch<NacosGrpcConnectionSnapshot>::Subscriber;

    NacosGrpcConnection(event::EventLoop &loop, const NacosClientConfig &config, const NacosClientOptions &options);
    ~NacosGrpcConnection();

    [[nodiscard]] async::Task<void> run() noexcept;
    void shutdown() noexcept;
    void notify_auth(const NacosAuthSnapshot &snapshot);
    void set_push_handler(NacosPushHandler handler) noexcept;

    [[nodiscard]] StateSubscriber subscribe_state();

    template<typename Request, typename Response>
    [[nodiscard]] async::Task<std::expected<void, NacosRpcError>> request(const Request &request, mem::BufPool &pool,
                                                                          Response &response) noexcept {
        FIBER_ASSERT(loop_->in_loop());
        auto lease = acquire_requester();
        if (!lease) {
            const NacosRpcErrorCode code = control_.stopping
                                                   ? NacosRpcErrorCode::Shutdown
                                                   : (!auth_ready() ? NacosRpcErrorCode::AuthenticationUnavailable
                                                                    : NacosRpcErrorCode::Transport);
            co_return std::unexpected(NacosRpcError{
                    .code = code,
                    .io_error = control_.stopping ? common::IoErr::Canceled
                                                  : (code == NacosRpcErrorCode::AuthenticationUnavailable
                                                             ? common::IoErr::Permission
                                                             : common::IoErr::NotConnected),
            });
        }
        auto metadata = current_metadata();
        if (!metadata) {
            release_requester(lease->generation);
            co_return std::unexpected(std::move(metadata.error()));
        }
        auto result = co_await lease->requester->request(request, *metadata, pool, response);
        release_requester(lease->generation);
        co_return result;
    }

private:
    struct Control {
        NacosAuthSnapshot auth;
        bool has_auth = false;
        bool stopping = false;
    };

    struct Endpoint {
        net::IpAddress ip;
        std::uint16_t port = 0;
        std::optional<std::size_t> server_index;
    };

    struct AttemptResult {
        NacosRpcError error;
        bool reached_ready = false;
    };

    [[nodiscard]] async::Task<AttemptResult> run_generation(const Endpoint &endpoint) noexcept;
    [[nodiscard]] async::DetachedTask monitor_client_close(Generation *generation) noexcept;
    [[nodiscard]] async::DetachedTask run_writer(Generation *generation) noexcept;
    [[nodiscard]] async::DetachedTask monitor_control(Generation *generation) noexcept;
    [[nodiscard]] async::DetachedTask run_heartbeat(Generation *generation) noexcept;
    [[nodiscard]] std::expected<bool, NacosRpcError>
    handle_inbound(Generation &generation, const proto::Payload &payload, bool handshaking) noexcept;
    [[nodiscard]] std::expected<NacosPayloadMetadata, NacosRpcError> current_metadata() const noexcept;
    [[nodiscard]] bool auth_ready() const noexcept;
    [[nodiscard]] std::chrono::milliseconds jittered(std::chrono::milliseconds delay) noexcept;
    [[nodiscard]] std::string authority(const Endpoint &endpoint) const;
    [[nodiscard]] std::optional<RequestLease> acquire_requester() noexcept;
    void release_requester(Generation *generation) noexcept;
    void publish_state(NacosGrpcConnectionState state, std::optional<NacosRpcError> error = std::nullopt,
                       std::optional<std::size_t> server_index = std::nullopt);
    void wake_control();
    void save_redirect(const dto::req::ConnectResetRequest &request) noexcept;

    event::EventLoop *loop_ = nullptr;
    const NacosClientConfig *config_ = nullptr;
    const NacosClientOptions *options_ = nullptr;
    Control control_;
    async::Watch<Control> control_watch_{Control{}};
    std::optional<async::Watch<Control>::Publisher> control_publisher_;
    NacosGrpcConnectionSnapshot snapshot_;
    async::Watch<NacosGrpcConnectionSnapshot> state_watch_{NacosGrpcConnectionSnapshot{}};
    std::optional<async::Watch<NacosGrpcConnectionSnapshot>::Publisher> state_publisher_;
    NacosPushHandler push_handler_;
    Generation *active_generation_ = nullptr;
    NacosRequester *active_requester_ = nullptr;
    std::string active_client_ip_;
    std::optional<Endpoint> redirect_;
    std::size_t preferred_server_index_ = 0;
    std::uint64_t random_state_ = 0x9e3779b97f4a7c15ull;
    bool run_active_ = false;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_RPC_NACOS_GRPC_CONNECTION_H
