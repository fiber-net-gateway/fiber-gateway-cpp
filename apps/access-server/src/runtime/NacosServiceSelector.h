#ifndef FIBER_ACCESS_SERVER_NACOS_SERVICE_SELECTOR_H
#define FIBER_ACCESS_SERVER_NACOS_SERVICE_SELECTOR_H

#include "../execution/ProxyRequestSender.h"
#include "AccessConfigWatcher.h"
#include "GrayMatchStore.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <async/Spawn.h>
#include <async/Task.h>
#include <async/WaitGroup.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <fiber/nacos/NamingService.h>

namespace fiber::access_server {

struct NacosServiceSelectorOptions {
    std::string group = std::string(kDefaultNacosGroup);
    std::string default_cluster = "default";
    std::string zone;
};

// Control-plane subscriptions are reconciled from each complete route
// snapshot. Request workers select through an atomically published immutable
// directory and never touch NamingService.
class NacosServiceSelector final : public common::NonCopyable, public common::NonMovable {
public:
    NacosServiceSelector(event::EventLoop &loop, nacos::NamingService &naming_service,
                         NacosServiceSelectorOptions options = {}, const GrayMatchStore *gray_match = nullptr);
    ~NacosServiceSelector();

    [[nodiscard]] std::expected<void, nacos::NamingServiceError> reconcile(const AccessRouteSnapshot &routes);
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] ProxyServiceSelector adapter() noexcept;
    [[nodiscard]] RouteSnapshotObserver route_observer() noexcept;
    [[nodiscard]] std::expected<ProxyUpstreamEndpoint, ProxyRequestError>
    select_endpoint(std::string_view service, std::optional<std::string_view> cluster = std::nullopt) noexcept;
    [[nodiscard]] std::size_t service_count() const noexcept { return entries_.size(); }
    [[nodiscard]] std::uint64_t naming_updates() const noexcept { return naming_updates_; }
    [[nodiscard]] std::uint64_t reconcile_failures() const noexcept { return reconcile_failures_; }

private:
    struct ServiceSnapshot;
    struct ServiceState;
    struct Entry;
    struct Directory;

    [[nodiscard]] static async::DetachedTask run(std::shared_ptr<Entry> entry) noexcept;
    static void route_snapshot_updated(void *context, std::shared_ptr<const AccessRouteSnapshot> snapshot) noexcept;
    [[nodiscard]] static std::expected<ProxyUpstreamEndpoint, ProxyRequestError>
    select(void *context, http::HttpExchange &exchange, std::string_view service,
           std::optional<std::string_view> cluster) noexcept;
    static void report(void *context, const ProxyUpstreamEndpoint &endpoint, bool success) noexcept;

    void apply(Entry &entry, const nacos::ServiceInfo &info);
    void publish_directory();
    void request_stop(Entry &entry) noexcept;
    [[nodiscard]] std::shared_ptr<const Directory> load_directory() const noexcept;
    void store_directory(std::shared_ptr<const Directory> directory, std::memory_order order) noexcept;

    event::EventLoop *loop_ = nullptr;
    nacos::NamingService *naming_service_ = nullptr;
    const GrayMatchStore *gray_match_ = nullptr;
    NacosServiceSelectorOptions options_;
    async::WaitGroup tasks_;
    std::map<std::string, std::shared_ptr<Entry>, std::less<>> entries_;
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const Directory>> directory_;
#else
    std::shared_ptr<const Directory> directory_;
#endif
    std::uint64_t naming_updates_ = 0;
    std::uint64_t reconcile_failures_ = 0;
    bool stopping_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_NACOS_SERVICE_SELECTOR_H
