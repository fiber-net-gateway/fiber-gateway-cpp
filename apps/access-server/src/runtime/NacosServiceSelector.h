#ifndef FIBER_ACCESS_SERVER_NACOS_SERVICE_SELECTOR_H
#define FIBER_ACCESS_SERVER_NACOS_SERVICE_SELECTOR_H

#include "../routing/ProxyAddressSelector.h"
#include "AccessConfigWatcher.h"
#include "SmoothWeightedRoundRobin.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <async/Task.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <fiber/nacos/discovery/ServiceDiscovery.h>

namespace fiber::access_server {

class NacosServiceSelector;
class AccessServiceState;
class NacosServiceAddressHandle;

struct AccessServiceOps {
    using State = AccessServiceState;
    using StatePtr = std::shared_ptr<State>;

    [[nodiscard]] StatePtr create(nacos::ServiceKeyView key, const std::shared_ptr<const nacos::ServiceInfo> &snapshot);
    [[nodiscard]] bool update(State &state, nacos::ServiceKeyView key,
                              const std::shared_ptr<const nacos::ServiceInfo> &snapshot);
    void on_change(State &state, nacos::ServiceKeyView key, nacos::ServiceChangeKind kind, bool changed) noexcept;
    void retire(State &state, nacos::ServiceKeyView key, nacos::ServiceRetireReason reason) noexcept;

    NacosServiceSelector *owner = nullptr;
    AccessUpstreamSwrr::Options swrr_options{};
    std::string zone;
};

using AccessServiceDiscovery = nacos::ServiceDiscovery<AccessServiceOps>;

struct NacosServiceSelectorOptions {
    std::string group = std::string(kDefaultNacosGroup);
    std::string default_cluster = "default";
    std::string zone;
};

// Control-plane subscriptions are reconciled from each complete route
// snapshot. Request workers select through per-service atomically published
// handles and never touch NamingService or ServiceDiscovery::Lease.
class NacosServiceSelector final : public common::NonCopyable, public common::NonMovable {
public:
    NacosServiceSelector(event::EventLoop &loop, nacos::NamingService &naming_service,
                         NacosServiceSelectorOptions options = {});
    ~NacosServiceSelector();

    [[nodiscard]] std::expected<void, nacos::NamingServiceError> reconcile(const AccessRouteSnapshot &routes);
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] ProxyAddressSelectorFactory address_selector_factory() noexcept;
    [[nodiscard]] RouteSnapshotObserver route_observer() noexcept;
    [[nodiscard]] std::size_t service_count() const noexcept { return entries_.size(); }
    [[nodiscard]] std::uint64_t naming_updates() const noexcept { return naming_updates_; }
    [[nodiscard]] std::uint64_t reconcile_failures() const noexcept { return reconcile_failures_; }

private:
    friend struct AccessServiceOps;

    struct ServiceEntry {
        AccessServiceDiscovery::Lease lease;
        std::shared_ptr<NacosServiceAddressHandle> handle;
    };

    static void route_snapshot_updated(void *context, std::shared_ptr<const AccessRouteSnapshot> snapshot) noexcept;
    [[nodiscard]] static std::shared_ptr<ProxyAddressSelector>
    create_address_selector(void *context, std::string service, std::optional<std::string> cluster);
    [[nodiscard]] std::shared_ptr<NacosServiceAddressHandle> get_or_create_handle(std::string_view service);
    void clear_handle(std::string_view service) noexcept;
    void publish_handles();

    event::EventLoop *loop_ = nullptr;
    NacosServiceSelectorOptions options_;
    AccessServiceDiscovery discovery_;
    std::map<std::string, ServiceEntry, std::less<>> entries_;
    std::map<std::string, std::weak_ptr<NacosServiceAddressHandle>, std::less<>> handles_;
    std::uint64_t naming_updates_ = 0;
    std::uint64_t reconcile_failures_ = 0;
    bool stopping_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_NACOS_SERVICE_SELECTOR_H
