#ifndef FIBER_AI_SERVER_RATE_LIMIT_CLUSTER_MEMBERSHIP_H
#define FIBER_AI_SERVER_RATE_LIMIT_CLUSTER_MEMBERSHIP_H

#include "RateLimitShardRing.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/Watch.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/event/EventLoop.h>
#include <fiber/nacos/NamingService.h>

namespace fiber::ai_server {

class RateLimitClusterMembership final : public common::NonCopyable, public common::NonMovable {
public:
    RateLimitClusterMembership(event::EventLoop &loop, nacos::NamingService &naming_service, RateLimitShardRing &ring,
                               std::string service_name, std::string group, std::string cluster_name);
    ~RateLimitClusterMembership();

    [[nodiscard]] std::expected<void, nacos::NamingServiceError> start(std::string advertise_ipv4, std::uint16_t port);
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] bool registered() const noexcept { return registered_.load(std::memory_order_acquire); }
    [[nodiscard]] std::string_view self_node_id() const noexcept { return self_node_id_; }

private:
    static void service_notify(void *context, const nacos::SubscriptionResult<nacos::ServiceInfo> &result) noexcept;
    [[nodiscard]] async::DetachedTask watch_registration() noexcept;
    void apply(const nacos::ServiceInfo &info);

    event::EventLoop *loop_ = nullptr;
    nacos::NamingService *naming_service_ = nullptr;
    RateLimitShardRing *ring_ = nullptr;
    std::string service_name_;
    std::string group_;
    std::string cluster_name_;
    std::string advertise_ipv4_;
    std::string self_node_id_;
    std::optional<nacos::Subscription<nacos::ServiceInfo>> subscription_;
    std::optional<nacos::InstanceRegistration> registration_;
    std::shared_ptr<const nacos::ServiceInfo> pending_service_;
    async::Watch<bool> stop_{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher_;
    async::WaitGroup tasks_;
    std::atomic<bool> registered_{false};
    std::uint64_t ring_version_ = 0;
    bool started_ = false;
    bool stopping_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_RATE_LIMIT_CLUSTER_MEMBERSHIP_H
