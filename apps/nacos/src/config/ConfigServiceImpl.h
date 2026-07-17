#ifndef FIBER_NACOS_CONFIG_CONFIG_SERVICE_IMPL_H
#define FIBER_NACOS_CONFIG_CONFIG_SERVICE_IMPL_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <vector>

#include <async/Spawn.h>
#include <async/WaitGroup.h>
#include <common/IntrusiveRbTree.h>
#include <event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NacosClientConfig.h>

#include "../rpc/NacosGrpcConnection.h"

namespace fiber::nacos::detail {

struct ConfigEntryCompare;

// Per-(data_id, group) subscription state. The key bytes (data_id, group) are
// tailed onto the same malloc block as the entry and exposed as string_views,
// so no std::string / ConfigKey allocation is needed. tree_hook links the entry
// into the registry RB tree; ref_count keeps it alive while indexed.
//
// ref_count is an intrusive reference count: the registry RB tree holds one
// reference (retained on insert, released on erase), mirroring the ownership
// the old std::unordered_map provided through its mapped value. Other holders
// (subscription leases, in-flight tasks) hold references via ConfigEntryPtr.
struct ConfigEntry {
    ConfigEntry();

    std::string_view data_id;
    std::string_view group;
    async::Watch<ConfigSnapshot> watch{ConfigSnapshot{}};
    std::optional<async::Watch<ConfigSnapshot>::Publisher> publisher;
    common::IntrusiveRbTreeHook tree_hook{};
    std::atomic<std::uint32_t> ref_count{0};
    std::size_t subscribers = 0;
    std::uint64_t registered_generation = 0;
    std::uint64_t query_sequence = 0;
    bool query_in_flight = false;
    bool dirty = false;
    bool closing = false;
    bool registration_in_flight = false;
    bool registration_dirty = false;

    void retain() noexcept { ref_count.fetch_add(1, std::memory_order_relaxed); }
    void release() noexcept {
        if (ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            this->~ConfigEntry();
            std::free(this);
        }
    }
};

struct ConfigEntryCompare {
    [[nodiscard]] bool operator()(const ConfigEntry *lhs, const ConfigEntry *rhs) const noexcept;
};

// Intrusive reference-counted handle for ConfigEntry. ConfigEntry is allocated
// as a single malloc block with its data_id/group bytes tailed onto it, so it
// cannot use std::make_shared / std::shared_ptr without a control-block
// indirection; the refcount lives in the entry itself.
class ConfigEntryPtr {
public:
    ConfigEntryPtr() noexcept = default;

    explicit ConfigEntryPtr(ConfigEntry *entry) noexcept : entry_(entry) {
        if (entry_ != nullptr) {
            entry_->retain();
        }
    }

    ConfigEntryPtr(const ConfigEntryPtr &other) noexcept : entry_(other.entry_) {
        if (entry_ != nullptr) {
            entry_->retain();
        }
    }

    ConfigEntryPtr(ConfigEntryPtr &&other) noexcept : entry_(other.entry_) { other.entry_ = nullptr; }

    ConfigEntryPtr &operator=(const ConfigEntryPtr &other) noexcept {
        if (this != &other) {
            if (other.entry_ != nullptr) {
                other.entry_->retain();
            }
            reset();
            entry_ = other.entry_;
        }
        return *this;
    }

    ConfigEntryPtr &operator=(ConfigEntryPtr &&other) noexcept {
        if (this != &other) {
            reset();
            entry_ = other.entry_;
            other.entry_ = nullptr;
        }
        return *this;
    }

    ~ConfigEntryPtr() { reset(); }

    [[nodiscard]] ConfigEntry &operator*() const noexcept { return *entry_; }
    [[nodiscard]] ConfigEntry *operator->() const noexcept { return entry_; }
    [[nodiscard]] ConfigEntry *get() const noexcept { return entry_; }
    [[nodiscard]] explicit operator bool() const noexcept { return entry_ != nullptr; }

    void reset() noexcept {
        if (entry_ != nullptr) {
            entry_->release();
            entry_ = nullptr;
        }
    }

    friend bool operator==(const ConfigEntryPtr &lhs, std::nullptr_t) noexcept { return lhs.entry_ == nullptr; }
    friend bool operator==(std::nullptr_t, const ConfigEntryPtr &rhs) noexcept { return rhs.entry_ == nullptr; }

private:
    ConfigEntry *entry_ = nullptr;
};

struct ConfigServiceLifetime {
    class ConfigServiceImpl *owner = nullptr;
};

struct ConfigSubscriptionLease {
    ConfigSubscriptionLease(std::shared_ptr<ConfigServiceLifetime> lifetime, ConfigEntryPtr entry) noexcept;
    ~ConfigSubscriptionLease();

    void close() noexcept;

    std::shared_ptr<ConfigServiceLifetime> lifetime;
    ConfigEntryPtr entry;
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
    void release_subscription(const ConfigEntryPtr &entry) noexcept;

private:
    using EntryPtr = ConfigEntryPtr;
    using Registry = common::IntrusiveRbTree<ConfigEntry, offsetof(ConfigEntry, tree_hook), ConfigEntryCompare>;

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
    [[nodiscard]] static ConfigEntry *entry_from_hook(common::IntrusiveRbTreeHook *hook) noexcept;
    [[nodiscard]] EntryPtr find_entry(std::string_view data_id, std::string_view group);
    void unlink_entry(ConfigEntry &entry) noexcept;
    void publish_snapshot(ConfigEntry &entry, ConfigSnapshot snapshot);
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
