#include "NacosServiceSelector.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include <async/Spawn.h>
#include <async/Watch.h>
#include <async/WhenAny.h>
#include <common/Assert.h>

namespace fiber::access_server {
namespace {

ProxyRequestError select_error(const char *message, common::IoErr io_error) noexcept {
    return ProxyRequestError{
            .code = ProxyRequestErrorCode::SelectUpstream,
            .io_error = io_error,
            .message = message,
    };
}

std::string make_host_header(std::string_view host, std::uint16_t port, const std::optional<net::IpAddress> &ip) {
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

struct NacosServiceSelector::ServiceSnapshot {
    struct Peer {
        std::string host;
        std::uint16_t port = 0;
        std::string host_header;
        std::optional<net::IpAddress> ip;
        std::string cluster;
        std::int64_t weight = 1;
        std::uint64_t selection_token = 0;
        bool primary_zone = true;
    };

    std::string service;
    std::uint64_t generation = 0;
    std::vector<Peer> peers;
};

struct NacosServiceSelector::ServiceState final : public common::NonCopyable, public common::NonMovable {
    struct SelectedPeer {
        std::shared_ptr<const ServiceSnapshot> snapshot;
        std::size_t index = 0;
    };

    [[nodiscard]] std::expected<SelectedPeer, ProxyRequestError> select(std::string_view cluster) noexcept {
        std::lock_guard guard(mutex);
        std::shared_ptr<const ServiceSnapshot> current = snapshot.load(std::memory_order_acquire);
        if (!current) {
            return std::unexpected(
                    select_error("service discovery has not received an initial value", common::IoErr::NotFound));
        }
        if (runtime_snapshot != current) {
            runtime_snapshot = current;
            current_weights.assign(current->peers.size(), 0);
        }

        bool has_primary = false;
        std::size_t candidate_count = 0;
        for (const ServiceSnapshot::Peer &peer: current->peers) {
            if (peer.cluster == cluster && peer.primary_zone) {
                has_primary = true;
            }
        }
        for (const ServiceSnapshot::Peer &peer: current->peers) {
            if (peer.cluster == cluster && (!has_primary || peer.primary_zone)) {
                ++candidate_count;
            }
        }

        std::size_t best = current->peers.size();
        std::int64_t best_weight = std::numeric_limits<std::int64_t>::min();
        std::int64_t total = 0;
        for (std::size_t i = 0; i < current->peers.size(); ++i) {
            const ServiceSnapshot::Peer &peer = current->peers[i];
            if (peer.cluster != cluster || (has_primary && !peer.primary_zone) ||
                (candidate_count > 1 && peer.selection_token == avoid_once)) {
                continue;
            }
            current_weights[i] += peer.weight;
            total += peer.weight;
            if (best == current->peers.size() || current_weights[i] > best_weight) {
                best = i;
                best_weight = current_weights[i];
            }
        }
        if (best == current->peers.size()) {
            return std::unexpected(select_error("no available service instance", common::IoErr::NotFound));
        }
        current_weights[best] -= total;
        avoid_once = 0;
        return SelectedPeer{
                .snapshot = std::move(current),
                .index = best,
        };
    }

    void report_failure(std::uint64_t selection_token) noexcept {
        std::lock_guard guard(mutex);
        avoid_once = selection_token;
    }

    std::atomic<std::shared_ptr<const ServiceSnapshot>> snapshot;
    std::mutex mutex;
    std::shared_ptr<const ServiceSnapshot> runtime_snapshot;
    std::vector<std::int64_t> current_weights;
    std::uint64_t avoid_once = 0;
};

struct NacosServiceSelector::Entry final : public common::NonCopyable, public common::NonMovable {
    Entry(NacosServiceSelector &value_owner, std::string value_service,
          nacos::Subscription<nacos::ServiceInfo> value_subscription) :
        owner(&value_owner), service(std::move(value_service)), subscription(std::move(value_subscription)),
        state(std::make_shared<ServiceState>()) {
        stop_publisher = stop.acquire_publisher();
        FIBER_ASSERT(stop_publisher.has_value());
    }

    NacosServiceSelector *owner = nullptr;
    std::string service;
    nacos::Subscription<nacos::ServiceInfo> subscription;
    std::shared_ptr<ServiceState> state;
    async::Watch<bool> stop{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher;
    std::uint64_t generation = 0;
    std::uint64_t next_selection_token = 0;
    bool stopping = false;
};

struct NacosServiceSelector::Directory {
    struct Item {
        std::string service;
        std::shared_ptr<ServiceState> state;
    };

    [[nodiscard]] std::shared_ptr<ServiceState> find(std::string_view service) const noexcept {
        const auto iterator =
                std::lower_bound(services.begin(), services.end(), service,
                                 [](const Item &item, std::string_view value) { return item.service < value; });
        if (iterator == services.end() || iterator->service != service) {
            return nullptr;
        }
        return iterator->state;
    }

    std::vector<Item> services;
};

NacosServiceSelector::NacosServiceSelector(event::EventLoop &loop, nacos::NamingService &naming_service,
                                           NacosServiceSelectorOptions options, const GrayMatchStore *gray_match) :
    loop_(&loop), naming_service_(&naming_service), gray_match_(gray_match), options_(std::move(options)) {
    directory_.store(std::make_shared<const Directory>(), std::memory_order_relaxed);
}

NacosServiceSelector::~NacosServiceSelector() {
    FIBER_ASSERT(entries_.empty());
    FIBER_ASSERT(tasks_.empty());
}

std::expected<void, nacos::NamingServiceError> NacosServiceSelector::reconcile(const AccessRouteSnapshot &routes) {
    FIBER_ASSERT(loop_->in_loop());
    if (stopping_) {
        return std::unexpected(nacos::NamingServiceError{
                .code = nacos::NamingServiceErrorCode::Shutdown,
                .io_error = common::IoErr::Canceled,
                .message = "access service selector is stopping",
        });
    }

    std::set<std::string, std::less<>> requested;
    for (const std::shared_ptr<const ProjectRouteSnapshot> &project: routes.projects()) {
        for (const CompiledRoute &route: project->routes()) {
            if (route.proxy && route.proxy->upstream_kind == ProxyUpstreamKind::Service) {
                requested.emplace(route.proxy->service);
            }
        }
    }

    std::vector<std::string> added;
    for (const std::string &service: requested) {
        if (entries_.contains(service)) {
            continue;
        }
        auto subscription = naming_service_->subscribe(service, options_.group);
        if (!subscription) {
            for (const std::string &added_service: added) {
                auto added_iterator = entries_.find(added_service);
                std::shared_ptr<Entry> retiring = std::move(added_iterator->second);
                entries_.erase(added_iterator);
                request_stop(*retiring);
            }
            ++reconcile_failures_;
            return std::unexpected(std::move(subscription.error()));
        }
        auto entry = std::make_shared<Entry>(*this, service, std::move(*subscription));
        auto [iterator, inserted] = entries_.emplace(service, entry);
        FIBER_ASSERT(inserted);
        (void) iterator;
        tasks_.add();
        async::spawn([entry = std::move(entry)]() mutable { return run(std::move(entry)); });
        added.push_back(service);
    }

    std::vector<std::string> removed;
    removed.reserve(entries_.size());
    for (const auto &[service, entry]: entries_) {
        (void) entry;
        if (!requested.contains(service)) {
            removed.push_back(service);
        }
    }
    for (const std::string &service: removed) {
        auto iterator = entries_.find(service);
        std::shared_ptr<Entry> retiring = std::move(iterator->second);
        entries_.erase(iterator);
        request_stop(*retiring);
    }
    publish_directory();
    return {};
}

async::Task<void> NacosServiceSelector::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (!stopping_) {
        stopping_ = true;
        for (auto &[service, entry]: entries_) {
            (void) service;
            request_stop(*entry);
        }
        directory_.store(std::make_shared<const Directory>(), std::memory_order_release);
    }
    co_await tasks_.join();
    entries_.clear();
}

ProxyServiceSelector NacosServiceSelector::adapter() noexcept {
    return ProxyServiceSelector{
            .context = this,
            .select = &NacosServiceSelector::select,
            .report = &NacosServiceSelector::report,
    };
}

RouteSnapshotObserver NacosServiceSelector::route_observer() noexcept {
    return RouteSnapshotObserver{
            .context = this,
            .on_update = &NacosServiceSelector::route_snapshot_updated,
    };
}

std::expected<ProxyUpstreamEndpoint, ProxyRequestError>
NacosServiceSelector::select_endpoint(std::string_view service, std::optional<std::string_view> cluster) noexcept {
    std::shared_ptr<const Directory> directory = directory_.load(std::memory_order_acquire);
    std::shared_ptr<ServiceState> state = directory->find(service);
    if (!state) {
        return std::unexpected(
                select_error("service is not present in the active route directory", common::IoErr::NotFound));
    }
    const std::string_view selected_cluster =
            cluster && !cluster->empty() ? *cluster : std::string_view(options_.default_cluster);
    auto selected = state->select(selected_cluster);
    if (!selected) {
        return std::unexpected(std::move(selected.error()));
    }

    const ServiceSnapshot::Peer &peer = selected->snapshot->peers[selected->index];
    return ProxyUpstreamEndpoint{
            .scheme = ProxyUpstreamScheme::Http,
            .host = peer.host,
            .port = peer.port,
            .host_header = peer.host_header,
            .ip_address = peer.ip,
            .owner = selected->snapshot,
            .selection_token = peer.selection_token,
    };
}

async::DetachedTask NacosServiceSelector::run(std::shared_ptr<Entry> entry) noexcept {
    auto stop = entry->stop.subscribe();
    auto stop_snapshot = stop.current();
    auto &subscriber = entry->subscription.subscriber();
    auto snapshot = subscriber.current();
    for (;;) {
        if (stop_snapshot.value && *stop_snapshot.value) {
            break;
        }
        if (snapshot.value) {
            if (snapshot.value->kind == nacos::ResultKind::Closed) {
                break;
            }
            if (snapshot.value->data && !entry->stopping) {
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

void NacosServiceSelector::route_snapshot_updated(void *context,
                                                  std::shared_ptr<const AccessRouteSnapshot> snapshot) noexcept {
    auto &self = *static_cast<NacosServiceSelector *>(context);
    auto reconciled = self.reconcile(*snapshot);
    (void) reconciled;
}

std::expected<ProxyUpstreamEndpoint, ProxyRequestError>
NacosServiceSelector::select(void *context, http::HttpExchange &exchange, std::string_view service,
                             std::optional<std::string_view> cluster) noexcept {
    auto &self = *static_cast<NacosServiceSelector *>(context);
    if (self.gray_match_ && self.gray_match_->matches(exchange)) {
        cluster = std::string_view("gray");
    }
    return self.select_endpoint(service, cluster);
}

void NacosServiceSelector::report(void *context, const ProxyUpstreamEndpoint &endpoint, bool success) noexcept {
    if (!success && endpoint.owner && endpoint.selection_token != 0) {
        auto &self = *static_cast<NacosServiceSelector *>(context);
        auto snapshot = std::static_pointer_cast<const ServiceSnapshot>(endpoint.owner);
        std::shared_ptr<const Directory> directory = self.directory_.load(std::memory_order_acquire);
        std::shared_ptr<ServiceState> state = directory->find(snapshot->service);
        if (state) {
            state->report_failure(endpoint.selection_token);
        }
    }
    // Endpoint reporting and pool algorithms are intentionally outside the
    // unified-access compatibility boundary. endpoint.owner still pins the
    // exact discovery generation until the request finishes.
}

void NacosServiceSelector::apply(Entry &entry, const nacos::ServiceInfo &info) {
    FIBER_ASSERT(loop_->in_loop());
    auto update = std::make_shared<ServiceSnapshot>();
    update->service = entry.service;
    update->generation = ++entry.generation;
    update->peers.reserve(info.hosts.size());
    for (const nacos::Instance &instance: info.hosts) {
        if (!instance.enabled || !instance.healthy || instance.ip.empty() || instance.port == 0 ||
            !std::isfinite(instance.weight) || instance.weight <= 0.0) {
            continue;
        }

        ServiceSnapshot::Peer peer;
        peer.host = instance.ip;
        peer.port = instance.port;
        net::IpAddress ip;
        if (net::IpAddress::parse(instance.ip, ip)) {
            peer.ip = ip;
        }
        peer.host_header = make_host_header(peer.host, peer.port, peer.ip);
        peer.cluster = instance.cluster_name;
        const std::size_t separator = peer.cluster.find('-');
        if (separator != std::string::npos) {
            const std::string_view instance_zone(peer.cluster.data(), separator);
            peer.primary_zone = !options_.zone.empty() && instance_zone == options_.zone;
            peer.cluster.erase(0, separator + 1);
        }
        const long double scaled = static_cast<long double>(instance.weight) * 100.0L;
        peer.weight = std::clamp<std::int64_t>(static_cast<std::int64_t>(scaled), 1,
                                               std::numeric_limits<std::int32_t>::max());
        peer.selection_token = ++entry.next_selection_token;
        if (peer.selection_token == 0) {
            peer.selection_token = ++entry.next_selection_token;
        }
        update->peers.push_back(std::move(peer));
    }
    entry.state->snapshot.store(std::move(update), std::memory_order_release);
    ++naming_updates_;
}

void NacosServiceSelector::publish_directory() {
    auto directory = std::make_shared<Directory>();
    directory->services.reserve(entries_.size());
    for (const auto &[service, entry]: entries_) {
        directory->services.push_back(Directory::Item{
                .service = service,
                .state = entry->state,
        });
    }
    directory_.store(std::move(directory), std::memory_order_release);
}

void NacosServiceSelector::request_stop(Entry &entry) noexcept {
    if (!entry.stopping) {
        entry.stopping = true;
        entry.stop_publisher->publish(true);
    }
}

} // namespace fiber::access_server
