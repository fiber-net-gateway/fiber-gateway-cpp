#include <fiber/nacos/discovery/ServiceDiscovery.h>

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include <async/Spawn.h>
#include <async/WhenAny.h>
#include <common/Assert.h>

namespace fiber::nacos {
namespace {

std::string make_authority(std::string_view host, std::uint16_t port, const std::optional<net::IpAddress> &ip) {
    std::array<char, 5> port_text{};
    const auto converted = std::to_chars(port_text.data(), port_text.data() + port_text.size(), port);
    std::string result;
    result.reserve(host.size() + 8);
    if (ip && ip->is_v6()) {
        result.push_back('[');
        result.append(host);
        result.push_back(']');
    } else {
        result.append(host);
    }
    result.push_back(':');
    result.append(port_text.data(), converted.ptr);
    return result;
}

} // namespace

struct ServiceDiscovery::Entry final : public common::NonCopyable, public common::NonMovable {
    Entry(ServiceDiscovery &value_owner, Key value_key, Subscription<ServiceInfo> value_subscription,
          LoadBalancer::Options options) :
        owner(&value_owner), key(std::move(value_key)), subscription(std::move(value_subscription)),
        load_balancer(std::make_shared<LoadBalancer>(std::move(options))) {
        stop_publisher = stop.acquire_publisher();
        FIBER_ASSERT(stop_publisher.has_value());
    }

    void request_stop() noexcept {
        if (!stopping) {
            stopping = true;
            stop_publisher->publish(true);
        }
    }

    ServiceDiscovery *owner = nullptr;
    Key key;
    Subscription<ServiceInfo> subscription;
    std::shared_ptr<LoadBalancer> load_balancer;
    async::Watch<bool> stop{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher;
    bool initialized = false;
    bool stopping = false;
};

ServiceDiscovery::Handle::Handle(ServiceDiscovery &owner, std::shared_ptr<Entry> entry) noexcept :
    owner_(&owner), entry_(std::move(entry)) {
    FIBER_ASSERT(entry_ != nullptr);
}

ServiceDiscovery::Handle::~Handle() { reset(); }

ServiceDiscovery::Handle::Handle(Handle &&other) noexcept :
    owner_(std::exchange(other.owner_, nullptr)), entry_(std::move(other.entry_)) {}

ServiceDiscovery::Handle &ServiceDiscovery::Handle::operator=(Handle &&other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        entry_ = std::move(other.entry_);
    }
    return *this;
}

LoadBalancer &ServiceDiscovery::Handle::load_balancer() const noexcept {
    FIBER_ASSERT(entry_ != nullptr);
    return *entry_->load_balancer;
}

std::shared_ptr<LoadBalancer> ServiceDiscovery::Handle::shared_load_balancer() const noexcept {
    FIBER_ASSERT(entry_ != nullptr);
    return entry_->load_balancer;
}

std::string_view ServiceDiscovery::Handle::service_name() const noexcept {
    FIBER_ASSERT(entry_ != nullptr);
    return entry_->key.service_name;
}

std::string_view ServiceDiscovery::Handle::group() const noexcept {
    FIBER_ASSERT(entry_ != nullptr);
    return entry_->key.group;
}

void ServiceDiscovery::Handle::reset() noexcept {
    if (!owner_) {
        FIBER_ASSERT(entry_ == nullptr);
        return;
    }
    ServiceDiscovery *owner = std::exchange(owner_, nullptr);
    std::shared_ptr<Entry> entry = std::move(entry_);
    owner->release(entry);
}

ServiceDiscovery::ServiceDiscovery(event::EventLoop &loop, NamingService &naming_service,
                                   ServiceDiscoveryOptions options, ServiceDiscoveryObserver observer) noexcept :
    loop_(&loop), naming_service_(&naming_service), options_(std::move(options)), observer_(observer) {}

ServiceDiscovery::~ServiceDiscovery() {
    FIBER_ASSERT(entries_.empty());
    FIBER_ASSERT(tasks_.empty());
}

std::expected<ServiceDiscovery::Handle, NamingServiceError> ServiceDiscovery::acquire(std::string service_name,
                                                                                      std::string group) {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping_) {
        return std::unexpected(NamingServiceError{
                .code = NamingServiceErrorCode::Shutdown,
                .io_error = common::IoErr::Canceled,
                .message = "service discovery is stopping",
        });
    }

    Key key{.service_name = std::move(service_name), .group = std::move(group)};
    const auto existing = entries_.find(key);
    if (existing != entries_.end()) {
        FIBER_ASSERT(existing->second.second != std::numeric_limits<std::size_t>::max());
        ++existing->second.second;
        return Handle(*this, existing->second.first);
    }

    auto subscription = naming_service_->subscribe(key.service_name, key.group);
    if (!subscription) {
        return std::unexpected(std::move(subscription.error()));
    }
    auto entry = std::make_shared<Entry>(*this, std::move(key), std::move(*subscription), options_.load_balancer);
    auto [it, inserted] = entries_.emplace(entry->key, std::pair(entry, 1));
    FIBER_ASSERT(inserted);
    (void) it;
    tasks_.add();
    async::spawn([entry]() mutable { return run(std::move(entry)); });
    return Handle(*this, std::move(entry));
}

async::Task<void> ServiceDiscovery::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (!stopping_) {
        stopping_ = true;
        for (auto &[key, value]: entries_) {
            (void) key;
            value.first->request_stop();
        }
    }
    co_await tasks_.join();
}

async::DetachedTask ServiceDiscovery::run(std::shared_ptr<Entry> entry) noexcept {
    auto stop = entry->stop.subscribe();
    auto stop_snapshot = stop.current();
    auto &subscriber = entry->subscription.subscriber();
    auto snapshot = subscriber.current();
    for (;;) {
        if (stop_snapshot.value && *stop_snapshot.value) {
            break;
        }
        if (snapshot.value) {
            if (snapshot.value->kind == ResultKind::Closed) {
                if (!entry->stopping && entry->owner->observer_.on_closed) {
                    entry->owner->observer_.on_closed(entry->owner->observer_.context, entry->key.service_name,
                                                      entry->key.group);
                }
                break;
            }
            if (snapshot.value->data) {
                entry->owner->apply(*entry, *snapshot.value->data);
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
    entry->subscription.close();
    entry->owner->tasks_.done();
}

void ServiceDiscovery::apply(Entry &entry, const ServiceInfo &info) {
    FIBER_ASSERT(loop_->in_loop());
    if (!contains(entry)) {
        return;
    }

    DiscoveredService update;
    update.service_name = entry.key.service_name;
    update.group = info.group_name.empty() ? entry.key.group : info.group_name;
    update.checksum = info.checksum;
    update.last_ref_time = info.last_ref_time;
    update.instances.reserve(info.hosts.size());
    for (const Instance &instance: info.hosts) {
        if (!instance.enabled || !instance.healthy || !std::isfinite(instance.weight) || instance.weight <= 0.0 ||
            instance.ip.empty() || instance.port == 0) {
            continue;
        }
        net::IpAddress ip;
        const bool parsed_ip = net::IpAddress::parse(instance.ip, ip);
        if (options_.require_ip && !parsed_ip) {
            continue;
        }
        update.instances.push_back(DiscoveredInstance{
                .instance_id = instance.instance_id,
                .host = instance.ip,
                .ip_address = parsed_ip ? std::optional(ip) : std::nullopt,
                .port = instance.port,
                .authority = make_authority(instance.ip, instance.port, parsed_ip ? std::optional(ip) : std::nullopt),
                .weight = instance.weight,
                .cluster_name = instance.cluster_name,
        });
    }

    const LoadBalancerUpdateResult result = entry.load_balancer->update_instances(std::move(update));
    const bool first_update = !entry.initialized;
    entry.initialized = true;
    if (observer_.on_update) {
        observer_.on_update(observer_.context, *entry.load_balancer, entry.key.service_name, entry.key.group,
                            first_update, result);
    }
}

void ServiceDiscovery::release(const std::shared_ptr<Entry> &entry) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(entry != nullptr);
    const auto it = entries_.find(entry->key);
    FIBER_ASSERT(it != entries_.end());
    FIBER_ASSERT(it->second.first == entry);
    FIBER_ASSERT(it->second.second > 0);
    if (--it->second.second != 0) {
        return;
    }
    std::shared_ptr<Entry> retiring = std::move(it->second.first);
    entries_.erase(it);
    retiring->request_stop();
}

bool ServiceDiscovery::contains(const Entry &entry) const noexcept {
    const auto it = entries_.find(entry.key);
    return it != entries_.end() && it->second.first.get() == &entry;
}

} // namespace fiber::nacos
