#ifndef FIBER_AI_SERVER_SERVICE_DISCOVERY_H
#define FIBER_AI_SERVER_SERVICE_DISCOVERY_H

#include "LoadBalancer.h"

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

namespace fiber::ai_server {

struct ServiceDiscoveryObserver {
    using UpdateFn = void (*)(void *context, LoadBalancer &load_balancer, bool first_update);

    void *context = nullptr;
    UpdateFn on_update = nullptr;
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
        void reset() noexcept;

    private:
        friend class ServiceDiscovery;

        Handle(ServiceDiscovery &owner, std::shared_ptr<Entry> entry) noexcept;

        ServiceDiscovery *owner_ = nullptr;
        std::shared_ptr<Entry> entry_;
    };

    ServiceDiscovery(event::EventLoop &loop, nacos::NamingService &naming_service,
                     ServiceDiscoveryObserver observer = {}) noexcept;
    ~ServiceDiscovery();

    [[nodiscard]] std::expected<Handle, nacos::NamingServiceError> acquire(std::string service_name, std::string group);
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
    void apply(Entry &entry, const nacos::ServiceInfo &info);
    void release(const std::shared_ptr<Entry> &entry) noexcept;
    [[nodiscard]] bool contains(const Entry &entry) const noexcept;

    event::EventLoop *loop_ = nullptr;
    nacos::NamingService *naming_service_ = nullptr;
    ServiceDiscoveryObserver observer_;
    async::WaitGroup tasks_;
    std::map<Key, std::pair<std::shared_ptr<Entry>, std::size_t>> entries_;
    bool stopping_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_SERVICE_DISCOVERY_H
