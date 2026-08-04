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

using WeightedInstance = AccessUpstreamSwrr::WeightedInstance;

struct AccessEndpointDefinition {
    std::string instance_id;
    std::string raw_cluster;
    std::string logical_cluster;
    WeightedInstance endpoint;
    bool preferred = false;
};

std::string make_authority(std::string_view host, std::uint16_t port, bool ipv6) {
    std::array<char, 5> port_text{};
    const auto converted = std::to_chars(port_text.data(), port_text.data() + port_text.size(), port);
    std::string result;
    result.reserve(host.size() + 8);
    if (ipv6) {
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

int compare_ip(const net::IpAddress &left, const net::IpAddress &right) noexcept {
    if (left.family() != right.family()) {
        return left.family() < right.family() ? -1 : 1;
    }
    if (left.is_v4()) {
        if (left.v4_bytes() != right.v4_bytes()) {
            return left.v4_bytes() < right.v4_bytes() ? -1 : 1;
        }
    } else {
        if (left.v6_bytes() != right.v6_bytes()) {
            return left.v6_bytes() < right.v6_bytes() ? -1 : 1;
        }
        if (left.scope_id() != right.scope_id()) {
            return left.scope_id() < right.scope_id() ? -1 : 1;
        }
    }
    return 0;
}

int compare_connection_key(const http::Http1ConnectionGroupKey &left,
                           const http::Http1ConnectionGroupKey &right) noexcept {
    if (left.host_kind() != right.host_kind()) {
        return left.host_kind() < right.host_kind() ? -1 : 1;
    }
    if (left.is_name()) {
        if (left.host_name() != right.host_name()) {
            return left.host_name() < right.host_name() ? -1 : 1;
        }
    } else {
        const int compared = compare_ip(left.ip_address(), right.ip_address());
        if (compared != 0) {
            return compared;
        }
    }
    if (left.port() != right.port()) {
        return left.port() < right.port() ? -1 : 1;
    }
    if (left.scheme() != right.scheme()) {
        return left.scheme() < right.scheme() ? -1 : 1;
    }
    return 0;
}

int compare_identity(const AccessEndpointDefinition &left, const AccessEndpointDefinition &right) noexcept {
    const int compared =
            compare_connection_key(left.endpoint.instance.connection_key, right.endpoint.instance.connection_key);
    if (compared != 0) {
        return compared;
    }
    if (left.raw_cluster != right.raw_cluster) {
        return left.raw_cluster < right.raw_cluster ? -1 : 1;
    }
    return 0;
}

bool same_definition(const AccessEndpointDefinition &left, const AccessEndpointDefinition &right) noexcept {
    return compare_identity(left, right) == 0 && left.instance_id == right.instance_id &&
           left.logical_cluster == right.logical_cluster && left.endpoint.instance == right.endpoint.instance &&
           left.endpoint.weight == right.endpoint.weight && left.preferred == right.preferred;
}

bool same_definitions(const std::vector<AccessEndpointDefinition> &left,
                      const std::vector<AccessEndpointDefinition> &right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (!same_definition(left[i], right[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

class AccessServiceState final : public common::NonCopyable, public common::NonMovable {
public:
    using Selection = AccessUpstreamSwrr::Selection;

    AccessServiceState(AccessUpstreamSwrr::Options options, std::string zone) :
        options_(std::move(options)), zone_(std::move(zone)) {}

    [[nodiscard]] bool update(const nacos::ServiceInfo &snapshot) {
        std::vector<AccessEndpointDefinition> next_definitions;
        next_definitions.reserve(snapshot.hosts.size());
        for (const nacos::ServiceInstance &instance: snapshot.hosts) {
            if (!instance.enabled || !instance.healthy || !std::isfinite(instance.weight) || instance.weight <= 0.0 ||
                instance.ip.empty() || instance.port == 0) {
                continue;
            }

            net::IpAddress ip;
            const bool parsed_ip = net::IpAddress::parse(instance.ip, ip);
            std::optional<http::Http1ConnectionGroupKey> connection_key =
                    parsed_ip ? std::optional(http::Http1ConnectionGroupKey::from_ip(
                                        ip, instance.port, http::Http1ConnectionGroupKey::Scheme::Http))
                              : http::Http1ConnectionGroupKey::from_name(instance.ip, instance.port,
                                                                         http::Http1ConnectionGroupKey::Scheme::Http);
            if (!connection_key) {
                continue;
            }
            const std::size_t separator = instance.cluster_name.find('-');
            const std::string_view logical_cluster =
                    separator == std::string_view::npos ? std::string_view(instance.cluster_name)
                                                        : std::string_view(instance.cluster_name).substr(separator + 1);
            const bool preferred = zone_.empty() || separator == std::string_view::npos ||
                                   std::string_view(instance.cluster_name).substr(0, separator) == zone_;
            next_definitions.push_back(AccessEndpointDefinition{
                    .instance_id = std::string(instance.instance_id),
                    .raw_cluster = std::string(instance.cluster_name),
                    .logical_cluster = std::string(logical_cluster),
                    .endpoint =
                            WeightedInstance{
                                    .instance =
                                            AccessUpstreamInstance{
                                                    .connection_key = std::move(*connection_key),
                                                    .authority = make_authority(instance.ip, instance.port,
                                                                                parsed_ip && ip.is_v6()),
                                            },
                                    .weight = instance.weight,
                            },
                    .preferred = preferred,
            });
        }
        std::sort(next_definitions.begin(), next_definitions.end(),
                  [](const AccessEndpointDefinition &left, const AccessEndpointDefinition &right) {
                      const int endpoint = compare_identity(left, right);
                      if (endpoint != 0) {
                          return endpoint < 0;
                      }
                      return left.instance_id < right.instance_id;
                  });
        next_definitions.erase(
                std::unique(next_definitions.begin(), next_definitions.end(),
                            [](const AccessEndpointDefinition &left, const AccessEndpointDefinition &right) {
                                return compare_identity(left, right) == 0;
                            }),
                next_definitions.end());

        std::lock_guard guard(mutex_);
        if (initialized_) {
            if (!checksum_.empty() || !snapshot.checksum.empty()) {
                if (!checksum_.empty() && checksum_ == snapshot.checksum) {
                    return false;
                }
            } else if (same_definitions(definitions_, next_definitions)) {
                return false;
            }
        }

        assign_selection_tokens(next_definitions);
        std::vector<PendingCluster> pending_clusters;
        for (const AccessEndpointDefinition &definition: next_definitions) {
            auto iterator = std::lower_bound(
                    pending_clusters.begin(), pending_clusters.end(), definition.logical_cluster,
                    [](const PendingCluster &cluster, std::string_view name) { return cluster.name < name; });
            if (iterator == pending_clusters.end() || iterator->name != definition.logical_cluster) {
                iterator = pending_clusters.insert(iterator, PendingCluster{.name = definition.logical_cluster});
            }
            auto &endpoints = definition.preferred ? iterator->preferred : iterator->fallback;
            endpoints.push_back(definition.endpoint);
        }

        std::vector<Cluster> next_clusters;
        next_clusters.reserve(pending_clusters.size());
        for (PendingCluster &pending: pending_clusters) {
            const auto current = find_cluster(pending.name);
            Cluster cluster{
                    .name = std::move(pending.name),
                    .preferred = current ? current->preferred : std::make_shared<AccessUpstreamSwrr>(options_),
                    .fallback = current ? current->fallback : std::make_shared<AccessUpstreamSwrr>(options_),
            };
            (void) cluster.preferred->update(std::move(pending.preferred));
            (void) cluster.fallback->update(std::move(pending.fallback));
            next_clusters.push_back(std::move(cluster));
        }
        for (const Cluster &cluster: clusters_) {
            const auto iterator =
                    std::lower_bound(next_clusters.begin(), next_clusters.end(), cluster.name,
                                     [](const Cluster &item, std::string_view name) { return item.name < name; });
            if (iterator == next_clusters.end() || iterator->name != cluster.name) {
                (void) cluster.preferred->update({});
                (void) cluster.fallback->update({});
            }
        }

        checksum_ = snapshot.checksum;
        definitions_ = std::move(next_definitions);
        clusters_ = std::move(next_clusters);
        initialized_ = true;
        return true;
    }

    [[nodiscard]] std::expected<Selection, SwrrSelectError>
    select(std::string_view cluster, std::span<const std::uint64_t> excluded_selection_tokens) noexcept {
        std::lock_guard guard(mutex_);
        const Cluster *selected_cluster = find_cluster(cluster);
        if (selected_cluster == nullptr) {
            return std::unexpected(SwrrSelectError::NoAvailableInstance);
        }
        auto preferred = selected_cluster->preferred->select(excluded_selection_tokens);
        if (preferred) {
            return preferred;
        }
        return selected_cluster->fallback->select(excluded_selection_tokens);
    }

private:
    struct PendingCluster {
        std::string name;
        std::vector<WeightedInstance> preferred;
        std::vector<WeightedInstance> fallback;
    };

    struct Cluster {
        std::string name;
        std::shared_ptr<AccessUpstreamSwrr> preferred;
        std::shared_ptr<AccessUpstreamSwrr> fallback;
    };

    void assign_selection_tokens(std::vector<AccessEndpointDefinition> &next) noexcept {
        std::size_t old_index = 0;
        std::size_t new_index = 0;
        while (old_index < definitions_.size() && new_index < next.size()) {
            const int comparison = compare_identity(definitions_[old_index], next[new_index]);
            if (comparison < 0) {
                ++old_index;
                continue;
            }
            if (comparison > 0) {
                assign_new_token(next[new_index++]);
                continue;
            }
            next[new_index++].endpoint.selection_token = definitions_[old_index++].endpoint.selection_token;
        }
        while (new_index < next.size()) {
            assign_new_token(next[new_index++]);
        }
    }

    void assign_new_token(AccessEndpointDefinition &definition) noexcept {
        FIBER_ASSERT(next_selection_token_ != std::numeric_limits<std::uint64_t>::max());
        definition.endpoint.selection_token = ++next_selection_token_;
    }

    [[nodiscard]] const Cluster *find_cluster(std::string_view name) const noexcept {
        const auto iterator =
                std::lower_bound(clusters_.begin(), clusters_.end(), name,
                                 [](const Cluster &cluster, std::string_view value) { return cluster.name < value; });
        return iterator == clusters_.end() || iterator->name != name ? nullptr : &*iterator;
    }

    AccessUpstreamSwrr::Options options_;
    std::string zone_;
    std::mutex mutex_;
    std::string checksum_;
    std::vector<AccessEndpointDefinition> definitions_;
    std::vector<Cluster> clusters_;
    std::uint64_t next_selection_token_ = 0;
    bool initialized_ = false;
};

AccessServiceOps::StatePtr AccessServiceOps::create(nacos::ServiceKeyView,
                                                    const std::shared_ptr<const nacos::ServiceInfo> &snapshot) {
    FIBER_ASSERT(snapshot != nullptr);
    auto state = std::make_shared<State>(swrr_options, zone);
    const bool changed = state->update(*snapshot);
    FIBER_ASSERT(changed);
    return state;
}

bool AccessServiceOps::update(State &state, nacos::ServiceKeyView,
                              const std::shared_ptr<const nacos::ServiceInfo> &snapshot) {
    FIBER_ASSERT(snapshot != nullptr);
    return state.update(*snapshot);
}

void AccessServiceOps::on_change(State &, nacos::ServiceKeyView, nacos::ServiceChangeKind kind, bool) noexcept {
    FIBER_ASSERT(owner != nullptr);
    ++owner->naming_updates_;
    if (kind == nacos::ServiceChangeKind::Initial) {
        owner->publish_directory();
    }
}

void AccessServiceOps::retire(State &, nacos::ServiceKeyView, nacos::ServiceRetireReason) noexcept {}

struct NacosServiceSelector::Directory {
    struct Item {
        std::string service;
        std::shared_ptr<AccessServiceState> load_balancer;
    };

    [[nodiscard]] std::shared_ptr<AccessServiceState> find(std::string_view service) const noexcept {
        const auto iterator =
                std::lower_bound(services.begin(), services.end(), service,
                                 [](const Item &item, std::string_view value) { return item.service < value; });
        if (iterator == services.end() || iterator->service != service) {
            return nullptr;
        }
        return iterator->load_balancer;
    }

    std::vector<Item> services;
};

NacosServiceSelector::NacosServiceSelector(event::EventLoop &loop, nacos::NamingService &naming_service,
                                           NacosServiceSelectorOptions options, const GrayMatchStore *gray_match) :
    loop_(&loop), gray_match_(gray_match), options_(std::move(options)),
    discovery_(loop, naming_service, AccessServiceOps{.owner = this, .zone = options_.zone}) {
    store_directory(std::make_shared<const Directory>(), std::memory_order_relaxed);
}

NacosServiceSelector::~NacosServiceSelector() { FIBER_ASSERT(entries_.empty()); }

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
        auto acquired = discovery_.acquire(service, options_.group);
        if (!acquired) {
            for (const std::string &added_service: added) {
                entries_.erase(added_service);
            }
            ++reconcile_failures_;
            return std::unexpected(std::move(acquired.error()));
        }
        auto [iterator, inserted] = entries_.emplace(service, std::move(*acquired));
        FIBER_ASSERT(inserted);
        (void) iterator;
        added.push_back(service);
    }

    std::vector<std::string> removed;
    removed.reserve(entries_.size());
    for (const auto &[service, handle]: entries_) {
        (void) handle;
        if (!requested.contains(service)) {
            removed.push_back(service);
        }
    }
    for (const std::string &service: removed) {
        entries_.erase(service);
    }
    publish_directory();
    return {};
}

async::Task<void> NacosServiceSelector::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (!stopping_) {
        stopping_ = true;
        store_directory(std::make_shared<const Directory>(), std::memory_order_release);
        entries_.clear();
    }
    co_await discovery_.shutdown();
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
NacosServiceSelector::select_endpoint(std::string_view service, std::optional<std::string_view> cluster,
                                      std::span<const std::uint64_t> excluded_selection_tokens) noexcept {
    std::shared_ptr<const Directory> directory = load_directory();
    std::shared_ptr<AccessServiceState> load_balancer = directory->find(service);
    if (!load_balancer) {
        return std::unexpected(
                select_error("service is not present in the active route directory", common::IoErr::NotFound));
    }

    const std::string_view selected_cluster =
            cluster && !cluster->empty() ? *cluster : std::string_view(options_.default_cluster);
    auto selected = load_balancer->select(selected_cluster, excluded_selection_tokens);
    if (!selected) {
        return std::unexpected(select_error("no available service instance", common::IoErr::NotFound));
    }

    const std::uint64_t selection_token = selected->selection_token();
    const AccessUpstreamInstance &instance = selected->instance();
    return ProxyUpstreamEndpoint{
            .connection_key = &instance.connection_key,
            .host_header = instance.authority,
            .service_selection = std::move(*selected),
            .selection_token = selection_token,
    };
}

void NacosServiceSelector::route_snapshot_updated(void *context,
                                                  std::shared_ptr<const AccessRouteSnapshot> snapshot) noexcept {
    auto &self = *static_cast<NacosServiceSelector *>(context);
    auto reconciled = self.reconcile(*snapshot);
    (void) reconciled;
}

std::expected<ProxyUpstreamEndpoint, ProxyRequestError>
NacosServiceSelector::select(void *context, http::HttpExchange &exchange, std::string_view service,
                             std::optional<std::string_view> cluster,
                             std::span<const std::uint64_t> excluded_selection_tokens) noexcept {
    auto &self = *static_cast<NacosServiceSelector *>(context);
    if (self.gray_match_ && self.gray_match_->matches(exchange)) {
        cluster = std::string_view("gray");
    }
    return self.select_endpoint(service, cluster, excluded_selection_tokens);
}

void NacosServiceSelector::report(void *, ProxyUpstreamEndpoint &endpoint, bool success) noexcept {
    if (endpoint.service_selection.pending()) {
        endpoint.service_selection.report(success);
    }
}

void NacosServiceSelector::publish_directory() {
    auto directory = std::make_shared<Directory>();
    directory->services.reserve(entries_.size());
    for (const auto &[service, handle]: entries_) {
        std::shared_ptr<AccessServiceState> state = handle.try_state();
        if (state != nullptr) {
            directory->services.push_back(Directory::Item{
                    .service = service,
                    .load_balancer = std::move(state),
            });
        }
    }
    store_directory(std::move(directory), std::memory_order_release);
}

std::shared_ptr<const NacosServiceSelector::Directory> NacosServiceSelector::load_directory() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    return directory_.load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(&directory_, std::memory_order_acquire);
#endif
}

void NacosServiceSelector::store_directory(std::shared_ptr<const Directory> directory,
                                           std::memory_order order) noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    directory_.store(std::move(directory), order);
#else
    std::atomic_store_explicit(&directory_, std::move(directory), order);
#endif
}

} // namespace fiber::access_server
