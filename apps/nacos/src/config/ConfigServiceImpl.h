#ifndef FIBER_NACOS_CONFIG_CONFIG_SERVICE_IMPL_H
#define FIBER_NACOS_CONFIG_CONFIG_SERVICE_IMPL_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <async/Spawn.h>
#include <async/WaitGroup.h>
#include <async/Watch.h>
#include <event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NacosAuthAccess.h>
#include <fiber/nacos/NacosClientConfig.h>

#include "../SubscriptionPool.h"
#include "../rpc/NacosGrpcConnection.h"

namespace fiber::nacos::detail {

// Protocol-specific per-entry state. Lives in the entry's ProtocolState slot;
// the shared subscription machinery (refcount, watch, tree hook, closed flag)
// is owned by SubscriptionEntry / SubscriptionPool.
struct ConfigProtocolState {
    std::uint64_t registered_generation = 0;
    std::uint64_t query_sequence = 0;
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
    using AuthSubscriber = async::Watch<NacosAuthAccess>::Subscriber;

    ConfigServiceImpl(event::EventLoop &loop, const NacosClientConfig &config, const NacosClientOptions &options,
                      AuthSubscriber auth_subscriber);
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
    [[nodiscard]] NacosGrpcConnection::StateSubscriber subscribe_connection_state();

private:
    using EntryPtr = ConfigEntryPtr;

    // ---- pool callbacks ----
    void on_subscription_add(EntryPtr entry);
    [[nodiscard]] RemoveDecision on_subscription_remove(EntryPtr entry);

    // ---- gRPC push handling ----
    [[nodiscard]] static std::expected<proto::Payload, NacosRpcError>
    handle_push(void *context, const proto::Payload &request, const NacosPayloadMetadata &metadata,
                std::size_t max_payload_bytes) noexcept;
    [[nodiscard]] std::expected<proto::Payload, NacosRpcError> handle_push(const proto::Payload &request,
                                                                           const NacosPayloadMetadata &metadata,
                                                                           std::size_t max_payload_bytes) noexcept;

    // ---- helpers ----
    [[nodiscard]] ConfigServiceError validate_key(std::string_view data_id, std::string_view group) const;
    [[nodiscard]] ConfigServiceError map_error(NacosRpcError error) const;
    [[nodiscard]] ConfigServiceError response_error(const dto::ResponseBase &response) const;
    [[nodiscard]] bool valid_key(std::string_view data_id, std::string_view group) const noexcept;
    [[nodiscard]] bool response_content_valid(const dto::resp::ConfigQueryResponse &response) const noexcept;
    void publish_value(ConfigEntry &entry, ConfigData value);
    void schedule_query(const EntryPtr &entry, std::uint64_t generation);
    [[nodiscard]] async::DetachedTask query_and_sync(EntryPtr entry, std::uint64_t generation,
                                                     std::uint64_t sequence) noexcept;
    void schedule_registration(std::vector<EntryPtr> entries, bool listen, std::uint64_t generation);
    void complete_registration(const EntryPtr &entry, bool listen, std::uint64_t generation, bool success);
    [[nodiscard]] async::DetachedTask register_entries(std::vector<EntryPtr> entries, bool listen,
                                                       std::uint64_t generation) noexcept;
    void register_all(std::uint64_t generation);
    void process_changed(const dto::resp::ConfigChangeBatchListenResponse &response, std::uint64_t generation);
    [[nodiscard]] async::DetachedTask run_connection() noexcept;
    [[nodiscard]] async::DetachedTask run_auth() noexcept;
    void apply_auth(const NacosAuthAccess &auth_access);
    void task_done() noexcept;

    event::EventLoop *loop_ = nullptr;
    const NacosClientConfig *config_ = nullptr;
    const NacosClientOptions *options_ = nullptr;
    NacosGrpcConnection connection_;
    AuthSubscriber auth_subscriber_;
    SubscriptionPool<ConfigEntry> pool_;
    async::WaitGroup tasks_;
    std::uint64_t active_generation_ = 0;
    bool run_active_ = false;
    bool stopping_ = false;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_CONFIG_CONFIG_SERVICE_IMPL_H
