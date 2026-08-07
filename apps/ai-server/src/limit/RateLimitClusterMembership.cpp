#include "RateLimitClusterMembership.h"
#include "../observability/AiServerLogCategories.h"

#include <cmath>
#include <utility>
#include <vector>

#include <fiber/async/WhenAny.h>
#include <fiber/common/Assert.h>
#include <fiber/log/Log.h>
#include <fiber/net/IpAddress.h>

namespace fiber::ai_server {
namespace {

DEFINE_LOGGER(LOG_RATE_LIMIT, kAiServerRateLimitLogger);

nacos::NamingServiceError invalid_argument(std::string message) {
    return nacos::NamingServiceError{
            .code = nacos::NamingServiceErrorCode::InvalidArgument,
            .io_error = common::IoErr::Invalid,
            .message = std::move(message),
    };
}

} // namespace

RateLimitClusterMembership::RateLimitClusterMembership(event::EventLoop &loop, nacos::NamingService &naming_service,
                                                       RateLimitShardRing &ring, std::string service_name,
                                                       std::string group, std::string cluster_name) :
    loop_(&loop), naming_service_(&naming_service), ring_(&ring), service_name_(std::move(service_name)),
    group_(std::move(group)), cluster_name_(std::move(cluster_name)) {
    stop_publisher_ = stop_.acquire_publisher();
    FIBER_ASSERT(stop_publisher_.has_value());
}

RateLimitClusterMembership::~RateLimitClusterMembership() {
    FIBER_ASSERT(!started_);
    FIBER_ASSERT(tasks_.empty());
}

std::expected<void, nacos::NamingServiceError> RateLimitClusterMembership::start(std::string advertise_ipv4,
                                                                                 std::uint16_t port) {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(!started_);
    auto node_id = java_self_service_node_id(advertise_ipv4, port);
    if (!node_id) {
        return std::unexpected(invalid_argument("rate limit advertise endpoint is invalid"));
    }

    advertise_ipv4_ = std::move(advertise_ipv4);
    self_node_id_ = std::move(*node_id);
    auto initial = ring_->update(++ring_version_, {
                                                          RateLimitNode{
                                                                  .node_id = self_node_id_,
                                                                  .host = advertise_ipv4_,
                                                                  .port = port,
                                                                  .weight = 1,
                                                                  .local = true,
                                                          },
                                                  });
    if (!initial) {
        return std::unexpected(invalid_argument("failed to initialize rate limit ring"));
    }

    auto subscribed = naming_service_->subscribe(service_name_, group_, &service_notify, this);
    if (!subscribed) {
        (void) ring_->update(++ring_version_, {});
        return std::unexpected(std::move(subscribed.error()));
    }
    nacos::Instance instance{
            .instance_id = self_node_id_,
            .ip = advertise_ipv4_,
            .port = port,
            .weight = 1.0,
            .healthy = true,
            .enabled = true,
            .ephemeral = true,
            .cluster_name = cluster_name_,
    };
    auto registered = naming_service_->registry(service_name_, group_, std::move(instance));
    if (!registered) {
        subscribed->close();
        pending_service_.reset();
        (void) ring_->update(++ring_version_, {});
        return std::unexpected(std::move(registered.error()));
    }

    subscription_.emplace(std::move(*subscribed));
    registration_.emplace(std::move(*registered));
    started_ = true;
    if (pending_service_) {
        apply(*pending_service_);
        pending_service_.reset();
    }
    tasks_.add();
    async::spawn([this]() { return watch_registration(); });
    LOG(LOG_RATE_LIMIT, INFO) << "token rate limit cluster started service=" << log::quoted(service_name_)
                              << " group=" << log::quoted(group_) << " cluster=" << log::quoted(cluster_name_)
                              << " node=" << log::quoted(self_node_id_);
    return {};
}

void RateLimitClusterMembership::service_notify(void *context,
                                                const nacos::SubscriptionResult<nacos::ServiceInfo> &result) noexcept {
    auto &self = *static_cast<RateLimitClusterMembership *>(context);
    if (result.kind == nacos::ResultKind::Closed) {
        if (self.subscription_) {
            self.subscription_->close();
        }
        return;
    }
    if (!result.data) {
        return;
    }
    if (!self.started_) {
        self.pending_service_ = result.data;
        return;
    }
    self.apply(*result.data);
}

async::DetachedTask RateLimitClusterMembership::watch_registration() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    auto stop = stop_.subscribe();
    auto stop_snapshot = stop.current();
    auto status = registration_->subscribe_status();
    auto snapshot = status.current();
    for (;;) {
        if (stop_snapshot.value && *stop_snapshot.value) {
            break;
        }
        if (snapshot.value) {
            registered_.store(snapshot.value->state == nacos::RegistrationState::Registered, std::memory_order_release);
            if (snapshot.value->state == nacos::RegistrationState::Failed) {
                LOG(LOG_RATE_LIMIT, WARN) << "token rate limit instance registration failed"
                                          << " node=" << log::quoted(self_node_id_);
            }
        }
        auto result =
                co_await async::when_any([&status, version = snapshot.version]() { return status.next(version); },
                                         [&stop, version = stop_snapshot.version]() { return stop.next(version); });
        if (result.is<1>()) {
            std::move(result).get<1>();
            break;
        }
        snapshot = std::move(result).get<0>();
        stop_snapshot = stop.current();
    }
    tasks_.done();
}

void RateLimitClusterMembership::apply(const nacos::ServiceInfo &info) {
    FIBER_ASSERT(loop_->in_loop());
    std::vector<RateLimitNode> nodes;
    nodes.reserve(info.hosts.size());
    for (const nacos::ServiceInstance &instance: info.hosts) {
        if (!instance.enabled || !instance.healthy || !std::isfinite(instance.weight) || instance.weight <= 0.0 ||
            instance.port == 0) {
            continue;
        }
        net::IpAddress address;
        if (!net::IpAddress::parse(instance.ip, address) || !address.is_v4() || address.is_unspecified() ||
            address.is_multicast()) {
            continue;
        }
        auto node_id = java_self_service_node_id(instance.ip, instance.port);
        if (!node_id) {
            continue;
        }
        bool duplicate = false;
        for (const RateLimitNode &node: nodes) {
            if (node.node_id == *node_id) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            LOG(LOG_RATE_LIMIT, WARN) << "duplicate token rate limit node ignored node=" << log::quoted(*node_id);
            continue;
        }
        nodes.push_back(RateLimitNode{
                .node_id = std::move(*node_id),
                .host = std::string(instance.ip),
                .port = instance.port,
                .weight = 1,
                .local = false,
        });
        nodes.back().local = nodes.back().node_id == self_node_id_;
    }
    auto updated = ring_->update(++ring_version_, std::move(nodes));
    if (!updated) {
        LOG(LOG_RATE_LIMIT, WARN) << "token rate limit ring update rejected node="
                                  << log::quoted(updated.error().node_id);
        return;
    }
    const auto current = ring_->snapshot();
    LOG(LOG_RATE_LIMIT, INFO) << "token rate limit ring updated version=" << current->version
                              << " nodes=" << current->nodes.size();
}

async::Task<void> RateLimitClusterMembership::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (!started_) {
        co_return;
    }
    if (!stopping_) {
        stopping_ = true;
        stop_publisher_->publish(true);
    }
    subscription_->close();
    co_await tasks_.join();

    auto status = registration_->subscribe_status();
    auto snapshot = status.current();
    registration_->close();
    while (!snapshot.value || snapshot.value->state != nacos::RegistrationState::Closed) {
        snapshot = co_await status.next(snapshot.version);
    }
    registration_.reset();
    subscription_.reset();
    pending_service_.reset();
    registered_.store(false, std::memory_order_release);
    (void) ring_->update(++ring_version_, {});
    started_ = false;
    LOG(LOG_RATE_LIMIT, INFO) << "token rate limit cluster stopped";
}

} // namespace fiber::ai_server
