#include <fiber/nacos/discovery/ServiceDiscovery.h>

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

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
    Entry(ServiceDiscovery &value_owner, Key value_key, LoadBalancer::Options options) :
        owner(&value_owner), key(std::move(value_key)),
        load_balancer(std::make_shared<LoadBalancer>(std::move(options))) {}

    void request_stop() noexcept {
        if (!stopping) {
            stopping = true;
            subscription.close();
        }
    }

    ServiceDiscovery *owner = nullptr;
    Key key;
    Subscription<ServiceInfo> subscription;
    std::shared_ptr<LoadBalancer> load_balancer;
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

ServiceDiscovery::~ServiceDiscovery() { FIBER_ASSERT(entries_.empty()); }

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

    auto entry = std::make_shared<Entry>(*this, std::move(key), options_.load_balancer);
    auto [it, inserted] = entries_.emplace(entry->key, std::pair(entry, 1));
    FIBER_ASSERT(inserted);
    auto subscription = naming_service_->subscribe(entry->key.service_name, entry->key.group, &on_notify, entry.get());
    if (!subscription) {
        entries_.erase(it);
        return std::unexpected(std::move(subscription.error()));
    }
    entry->subscription = std::move(*subscription);
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
    co_return;
}

void ServiceDiscovery::on_notify(void *context, const SubscriptionResult<ServiceInfo> &result) noexcept {
    auto *entry = static_cast<Entry *>(context);
    FIBER_ASSERT(entry != nullptr);
    ServiceDiscovery *owner = entry->owner;
    FIBER_ASSERT(owner != nullptr);

    const auto found = owner->entries_.find(entry->key);
    if (found == owner->entries_.end() || found->second.first.get() != entry) {
        return;
    }
    std::shared_ptr<Entry> hold = found->second.first;
    if (result.kind == ResultKind::Closed) {
        if (!hold->stopping && owner->observer_.on_closed) {
            owner->observer_.on_closed(owner->observer_.context, hold->key.service_name, hold->key.group);
        }
        hold->request_stop();
        return;
    }
    if (result.data) {
        owner->apply(*hold, *result.data);
    }
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
    for (const ServiceInstance &instance: info.hosts) {
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
                .instance_id = std::string(instance.instance_id),
                .host = std::string(instance.ip),
                .ip_address = parsed_ip ? std::optional(ip) : std::nullopt,
                .port = instance.port,
                .authority = make_authority(instance.ip, instance.port, parsed_ip ? std::optional(ip) : std::nullopt),
                .weight = instance.weight,
                .cluster_name = std::string(instance.cluster_name),
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
