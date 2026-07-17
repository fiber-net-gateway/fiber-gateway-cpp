#ifndef FIBER_NACOS_CONFIG_CONFIG_SERVICE_IMPL_H
#define FIBER_NACOS_CONFIG_CONFIG_SERVICE_IMPL_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <async/Spawn.h>
#include <async/WaitGroup.h>
#include <event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NacosClientConfig.h>

#include "../rpc/NacosGrpcConnection.h"

namespace fiber::nacos::detail {

struct ConfigKey {
    std::string data_id;
    std::string group;

    [[nodiscard]] bool operator==(const ConfigKey &) const noexcept = default;
};

struct ConfigKeyView {
    std::string_view data_id;
    std::string_view group;
};

struct ConfigKeyHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(const ConfigKey &key) const noexcept;
    [[nodiscard]] std::size_t operator()(ConfigKeyView key) const noexcept;
};

struct ConfigKeyEqual {
    using is_transparent = void;

    [[nodiscard]] bool operator()(const ConfigKey &lhs, const ConfigKey &rhs) const noexcept;
    [[nodiscard]] bool operator()(const ConfigKey &lhs, ConfigKeyView rhs) const noexcept;
    [[nodiscard]] bool operator()(ConfigKeyView lhs, const ConfigKey &rhs) const noexcept;
};

struct ConfigEntry {
    explicit ConfigEntry(ConfigKey key);

    ConfigKey key;
    async::Watch<ConfigSnapshot> watch{ConfigSnapshot{}};
    std::optional<async::Watch<ConfigSnapshot>::Publisher> publisher;
    ConfigSnapshot snapshot;
    std::size_t subscribers = 0;
    std::uint64_t registered_generation = 0;
    std::uint64_t query_sequence = 0;
    bool query_in_flight = false;
    bool dirty = false;
    bool closing = false;
    bool registration_in_flight = false;
    bool registration_dirty = false;
};

struct ConfigServiceLifetime {
    class ConfigServiceImpl *owner = nullptr;
};

struct ConfigSubscriptionLease {
    ConfigSubscriptionLease(std::shared_ptr<ConfigServiceLifetime> lifetime,
                            std::shared_ptr<ConfigEntry> entry) noexcept;
    ~ConfigSubscriptionLease();

    void close() noexcept;

    std::shared_ptr<ConfigServiceLifetime> lifetime;
    std::shared_ptr<ConfigEntry> entry;
    bool closed = false;
};

class ConfigServiceImpl final : public ConfigService {
public:
    ConfigServiceImpl(event::EventLoop &loop, const NacosClientConfig &config, const NacosClientOptions &options,
                      NacosGrpcConnection &connection);
    ~ConfigServiceImpl() override;

    [[nodiscard]] async::Task<std::expected<std::optional<ConfigData>, ConfigServiceError>>
    get_config(std::string data_id, std::string group) noexcept override;
    [[nodiscard]] async::Task<std::expected<void, ConfigServiceError>>
    publish(std::string data_id, std::string group, std::string content, ConfigType type,
            std::optional<std::string> cas_md5 = std::nullopt) noexcept override;
    [[nodiscard]] async::Task<std::expected<void, ConfigServiceError>>
    remove_config(std::string data_id, std::string group) noexcept override;
    [[nodiscard]] std::expected<ConfigSubscription, ConfigServiceError> subscribe(std::string_view data_id,
                                                                                  std::string_view group) override;

    [[nodiscard]] async::Task<void> run() noexcept;
    void shutdown() noexcept;
    void release_subscription(const std::shared_ptr<ConfigEntry> &entry) noexcept;

private:
    using EntryPtr = std::shared_ptr<ConfigEntry>;
    using Registry = std::unordered_map<ConfigKey, EntryPtr, ConfigKeyHash, ConfigKeyEqual>;

    [[nodiscard]] static std::expected<proto::Payload, NacosRpcError>
    handle_push(void *context, const proto::Payload &request, const NacosPayloadMetadata &metadata,
                std::size_t max_payload_bytes) noexcept;
    [[nodiscard]] std::expected<proto::Payload, NacosRpcError> handle_push(const proto::Payload &request,
                                                                           const NacosPayloadMetadata &metadata,
                                                                           std::size_t max_payload_bytes) noexcept;
    [[nodiscard]] ConfigServiceError validate_key(std::string_view data_id, std::string_view group) const;
    [[nodiscard]] ConfigServiceError map_error(NacosRpcError error) const;
    [[nodiscard]] ConfigServiceError response_error(const dto::ResponseBase &response) const;
    [[nodiscard]] bool valid_key(std::string_view data_id, std::string_view group) const noexcept;
    [[nodiscard]] bool response_content_valid(const dto::resp::ConfigQueryResponse &response) const noexcept;
    [[nodiscard]] EntryPtr find_entry(std::string_view data_id, std::string_view group);
    void publish_snapshot(const EntryPtr &entry, ConfigSnapshot snapshot);
    void schedule_query(const EntryPtr &entry, std::uint64_t generation);
    [[nodiscard]] async::DetachedTask query_and_sync(EntryPtr entry, std::uint64_t generation,
                                                     std::uint64_t sequence) noexcept;
    void schedule_registration(std::vector<EntryPtr> entries, bool listen, std::uint64_t generation);
    void complete_registration(const EntryPtr &entry, bool listen, std::uint64_t generation, bool success);
    [[nodiscard]] async::DetachedTask register_entries(std::vector<EntryPtr> entries, bool listen,
                                                       std::uint64_t generation) noexcept;
    void register_all(std::uint64_t generation);
    void process_changed(const dto::resp::ConfigChangeBatchListenResponse &response, std::uint64_t generation);
    void task_done() noexcept;

    event::EventLoop *loop_ = nullptr;
    const NacosClientConfig *config_ = nullptr;
    const NacosClientOptions *options_ = nullptr;
    NacosGrpcConnection *connection_ = nullptr;
    Registry entries_;
    async::WaitGroup tasks_;
    std::shared_ptr<ConfigServiceLifetime> lifetime_;
    std::uint64_t active_generation_ = 0;
    bool run_active_ = false;
    bool stopping_ = false;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_CONFIG_CONFIG_SERVICE_IMPL_H
