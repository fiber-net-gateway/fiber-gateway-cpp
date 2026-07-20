#ifndef FIBER_NACOS_CONFIG_CONFIG_SERVICE_IMPL_H
#define FIBER_NACOS_CONFIG_CONFIG_SERVICE_IMPL_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <async/Spawn.h>
#include <async/WaitGroup.h>
#include <async/Watch.h>
#include <event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NacosAuthAccess.h>
#include <fiber/nacos/NacosClientConfig.h>

#include "../SubscriptionPool.h"
#include "../rpc/NacosBiRequestHandler.h"
#include "../rpc/NacosRpc.h"

namespace fiber::nacos::detail {

// Protocol-specific per-entry state. Lives in the entry's ProtocolState slot;
// the shared subscription machinery (refcount, watch, tree hook, closed flag)
// is owned by SubscriptionEntry / SubscriptionPool.
struct ConfigProtocolState {
    std::uint64_t query_sequence = 0;
    bool registered = false;
    bool query_in_flight = false;
    bool dirty = false;
    bool registration_in_flight = false;
    bool registration_dirty = false;
    // Replaces the old entry-wide `closing`: "this entry is draining, stop
    // issuing new registration/query work for it". Protocol-only.
    bool draining = false;
};

using ConfigEntry = SubscriptionEntry<ConfigData, ConfigProtocolState>;
using ConfigEntryPtr = EntryPtr<ConfigEntry>;
using ConfigResult = SubscriptionResult<ConfigData>;

class ConfigServiceImpl final : public ConfigService {
public:
    using AuthWatch = async::Watch<NacosAuthAccess>;
    using ReadySubscriber = async::Watch<bool>::Subscriber;

    ConfigServiceImpl(event::EventLoop &loop, const NacosClientConfig &config, const NacosClientOptions &options,
                      AuthWatch &auth_watch);
    ~ConfigServiceImpl() override;

    [[nodiscard]] static bool valid_options(const NacosClientOptions &options) noexcept;

    [[nodiscard]] async::Task<std::expected<std::optional<ConfigData>, ConfigServiceError>>
    get_config(std::string data_id, std::string group) noexcept override;
    [[nodiscard]] async::Task<std::expected<void, ConfigServiceError>>
    publish(std::string data_id, std::string group, std::string content, ConfigType type,
            std::optional<std::string> cas_md5 = std::nullopt) noexcept override;
    [[nodiscard]] async::Task<std::expected<void, ConfigServiceError>>
    remove_config(std::string data_id, std::string group) noexcept override;
    [[nodiscard]] std::expected<Subscription<ConfigData>, ConfigServiceError>
    subscribe(std::string_view data_id, std::string_view group) override;

    [[nodiscard]] async::Task<void> run() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] ReadySubscriber subscribe_connection_ready();

private:
    using EntryPtr = ConfigEntryPtr;

    struct AttemptResult {
        NacosRpcCloseResult close;
        bool reached_ready = false;
    };

    // ---- pool callbacks ----
    void on_subscription_add(EntryPtr entry);
    [[nodiscard]] RemoveDecision on_subscription_remove(EntryPtr entry);

    // ---- gRPC push handling ----
    [[nodiscard]] static async::Task<common::IoResult<dto::resp::ConfigChangeNotifyResponse>>
    handle_config_change(void *context, NacosServerRequestContext &,
                         const dto::req::ConfigChangeNotifyRequest &request) noexcept;

    // ---- helpers ----
    [[nodiscard]] ConfigServiceError validate_key(std::string_view data_id, std::string_view group) const;
    [[nodiscard]] ConfigServiceError map_error(NacosRpcError error) const;
    [[nodiscard]] ConfigServiceError response_error(const dto::ResponseBase &response) const;
    [[nodiscard]] bool valid_key(std::string_view data_id, std::string_view group) const noexcept;
    [[nodiscard]] bool response_content_valid(const dto::resp::ConfigQueryResponse &response) const noexcept;
    void publish_value(ConfigEntry &entry, ConfigData value);
    void schedule_query(const EntryPtr &entry);
    [[nodiscard]] async::DetachedTask query_and_sync(EntryPtr entry, std::uint64_t sequence) noexcept;
    void schedule_registration(std::vector<EntryPtr> entries, bool listen);
    void complete_registration(const EntryPtr &entry, bool listen, bool success);
    [[nodiscard]] async::DetachedTask register_entries(std::vector<EntryPtr> entries, bool listen) noexcept;
    void register_all();
    void process_changed(const dto::resp::ConfigChangeBatchListenResponse &response);
    void reset_connection_state();
    void set_rpc_ready(bool ready);
    [[nodiscard]] async::Task<void> run_connection() noexcept;
    [[nodiscard]] async::Task<AttemptResult> run_attempt(NacosRpcEndpoint endpoint) noexcept;
    [[nodiscard]] async::DetachedTask run_redo(std::uint64_t ready_version) noexcept;
    [[nodiscard]] async::Task<void> wait_backoff(std::chrono::milliseconds delay) noexcept;
    [[nodiscard]] std::chrono::milliseconds jittered(std::chrono::milliseconds delay) noexcept;
    void task_done() noexcept;

    template<typename Request, typename Response>
    [[nodiscard]] async::Task<std::expected<void, NacosRpcError>>
    request_rpc(const Request &request, mem::BufPool &pool, Response &response) noexcept {
        FIBER_ASSERT(loop_->in_loop());
        if (stopping_) {
            co_return std::unexpected(shutdown_rpc_error());
        }
        if (!rpc_ready_ || !rpc_) {
            co_return std::unexpected(not_connected_rpc_error());
        }
        auto result = co_await rpc_->request(request, pool, response);
        if (!result && result.error().code == NacosRpcErrorCode::Shutdown && !stopping_) {
            co_return std::unexpected(not_connected_rpc_error());
        }
        co_return std::move(result);
    }

    [[nodiscard]] static NacosRpcError shutdown_rpc_error();
    [[nodiscard]] static NacosRpcError not_connected_rpc_error();

    event::EventLoop *loop_ = nullptr;
    const NacosClientConfig *config_ = nullptr;
    const NacosClientOptions *options_ = nullptr;
    AuthWatch *auth_watch_ = nullptr;
    NacosBiRequestHandler handlers_;
    std::optional<NacosRpc> rpc_;
    SubscriptionPool<ConfigEntry> pool_;
    async::WaitGroup tasks_;
    async::Watch<bool> ready_watch_{false};
    std::optional<async::Watch<bool>::Publisher> ready_publisher_;
    async::Watch<bool> wake_watch_{false};
    std::optional<async::Watch<bool>::Publisher> wake_publisher_;
    std::optional<NacosRpcEndpoint> redirect_;
    std::size_t preferred_server_index_ = 0;
    std::uint64_t random_state_ = 0x9e3779b97f4a7c15ull;
    bool rpc_ready_ = false;
    bool run_active_ = false;
    bool stopping_ = false;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_CONFIG_CONFIG_SERVICE_IMPL_H
