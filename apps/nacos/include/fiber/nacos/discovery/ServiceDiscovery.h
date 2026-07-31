#ifndef FIBER_NACOS_DISCOVERY_SERVICE_DISCOVERY_H
#define FIBER_NACOS_DISCOVERY_SERVICE_DISCOVERY_H

#include "ServiceLoadBalancer.h"

#include <cstddef>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <async/Spawn.h>
#include <async/Task.h>
#include <async/WaitGroup.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <fiber/nacos/NamingService.h>

namespace fiber::nacos {

struct ServiceDiscoveryOptions {
    LoadBalancer::Options load_balancer;
    // IP-only consumers can reject hostname instances at the control-plane
    // boundary. Other consumers receive both the raw host and optional IP.
    bool require_ip = false;
};

// Callbacks run on the NamingService owner EventLoop.
struct ServiceDiscoveryObserver {
    using UpdateFn = void (*)(void *context, LoadBalancer &load_balancer, std::string_view service_name,
                              std::string_view group, bool first_update, LoadBalancerUpdateResult result);
    using ClosedFn = void (*)(void *context, std::string_view service_name, std::string_view group);

    void *context = nullptr;
    UpdateFn on_update = nullptr;
    ClosedFn on_closed = nullptr;
};

class ServiceDiscovery final : public common::NonCopyable, public common::NonMovable {
private:
    struct Entry;

public:
    class Handle final {
    public:
        Handle() noexcept = default;
        ~Handle();

        Handle(const Handle &) = delete;
        Handle &operator=(const Handle &) = delete;
        Handle(Handle &&other) noexcept;
        Handle &operator=(Handle &&other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept { return entry_ != nullptr; }
        [[nodiscard]] LoadBalancer &load_balancer() const noexcept;
        [[nodiscard]] std::shared_ptr<LoadBalancer> shared_load_balancer() const noexcept;
        [[nodiscard]] std::string_view service_name() const noexcept;
        [[nodiscard]] std::string_view group() const noexcept;
        // Owner-EventLoop-only. Handles must be reset before ServiceDiscovery
        // is destroyed.
        void reset() noexcept;

    private:
        friend class ServiceDiscovery;

        Handle(ServiceDiscovery &owner, std::shared_ptr<Entry> entry) noexcept;

        ServiceDiscovery *owner_ = nullptr;
        std::shared_ptr<Entry> entry_;
    };

    ServiceDiscovery(event::EventLoop &loop, NamingService &naming_service, ServiceDiscoveryOptions options = {},
                     ServiceDiscoveryObserver observer = {}) noexcept;
    ~ServiceDiscovery();

    // acquire(), Handle release, and shutdown() are owner-EventLoop-only.
    [[nodiscard]] std::expected<Handle, NamingServiceError> acquire(std::string service_name, std::string group);
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    struct Key {
        std::string service_name;
        std::string group;

        friend bool operator<(const Key &left, const Key &right) noexcept {
            if (left.service_name != right.service_name) {
                return left.service_name < right.service_name;
            }
            return left.group < right.group;
        }
    };

    [[nodiscard]] static async::DetachedTask run(std::shared_ptr<Entry> entry) noexcept;
    void apply(Entry &entry, const ServiceInfo &info);
    void release(const std::shared_ptr<Entry> &entry) noexcept;
    [[nodiscard]] bool contains(const Entry &entry) const noexcept;

    event::EventLoop *loop_ = nullptr;
    NamingService *naming_service_ = nullptr;
    ServiceDiscoveryOptions options_;
    ServiceDiscoveryObserver observer_;
    async::WaitGroup tasks_;
    std::map<Key, std::pair<std::shared_ptr<Entry>, std::size_t>> entries_;
    bool stopping_ = false;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_DISCOVERY_SERVICE_DISCOVERY_H
