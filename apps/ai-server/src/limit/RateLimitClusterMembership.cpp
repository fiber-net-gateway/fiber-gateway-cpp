#include "RateLimitClusterMembership.h"

#include <cmath>
#include <utility>
#include <vector>

#include <async/WhenAny.h>
#include <common/Assert.h>
#include <log/Log.h>
#include <net/IpAddress.h>

namespace fiber::ai_server {
namespace {

DEFINE_LOGGER(LOG_RATE_LIMIT, "ai_server.rate_limit");

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
                                                       std::string group) :
    loop_(&loop), naming_service_(&naming_service), ring_(&ring), service_name_(std::move(service_name)),
    group_(std::move(group)) {
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

    auto subscribed = naming_service_->subscribe(service_name_, group_);
    if (!subscribed) {
        return std::unexpected(std::move(subscribed.error()));
    }
    nacos::Instance instance{
            .instance_id = *node_id,
            .ip = advertise_ipv4,
            .port = port,
            .weight = 1.0,
            .healthy = true,
            .enabled = true,
            .ephemeral = true,
    };
    auto registered = naming_service_->registry(service_name_, group_, std::move(instance));
    if (!registered) {
        subscribed->close();
        return std::unexpected(std::move(registered.error()));
    }

    advertise_ipv4_ = std::move(advertise_ipv4);
    self_node_id_ = std::move(*node_id);
    subscription_.emplace(std::move(*subscribed));
    registration_.emplace(std::move(*registered));
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
        registration_->close();
        registration_.reset();
        subscription_->close();
        subscription_.reset();
        return std::unexpected(invalid_argument("failed to initialize rate limit ring"));
    }

    started_ = true;
    tasks_.add(2);
    async::spawn([this]() { return watch_service(); });
    async::spawn([this]() { return watch_registration(); });
    LOG(LOG_RATE_LIMIT, INFO) << "token rate limit cluster started service=" << log::quoted(service_name_)
                              << " group=" << log::quoted(group_) << " node=" << log::quoted(self_node_id_);
    return {};
}

async::DetachedTask RateLimitClusterMembership::watch_service() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    auto stop = stop_.subscribe();
    auto stop_snapshot = stop.current();
    auto &subscriber = subscription_->subscriber();
    auto snapshot = subscriber.current();
    for (;;) {
        if (stop_snapshot.value && *stop_snapshot.value) {
            break;
        }
        if (snapshot.value) {
            if (snapshot.value->kind == nacos::ResultKind::Closed) {
                break;
            }
            if (snapshot.value->data) {
                apply(*snapshot.value->data);
            }
        }
        auto result = co_await async::when_any(
                [&subscriber, version = snapshot.version]() { return subscriber.next(version); },
                [&stop, version = stop_snapshot.version]() { return stop.next(version); });
        if (result.is<1>()) {
            std::move(result).get<1>();
            break;
        }
        snapshot = std::move(result).get<0>();
        stop_snapshot = stop.current();
    }
    subscription_->close();
    tasks_.done();
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
    for (const nacos::Instance &instance: info.hosts) {
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
                .host = instance.ip,
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
    co_await tasks_.join();

    auto status = registration_->subscribe_status();
    auto snapshot = status.current();
    registration_->close();
    while (!snapshot.value || snapshot.value->state != nacos::RegistrationState::Closed) {
        snapshot = co_await status.next(snapshot.version);
    }
    registration_.reset();
    subscription_.reset();
    registered_.store(false, std::memory_order_release);
    (void) ring_->update(++ring_version_, {});
    started_ = false;
    LOG(LOG_RATE_LIMIT, INFO) << "token rate limit cluster stopped";
}

} // namespace fiber::ai_server
