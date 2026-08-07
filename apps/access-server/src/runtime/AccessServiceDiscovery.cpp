#include "AccessServiceDiscovery.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

ProxyAddressSelectError select_error(ProxyAddressSelectErrorCode code, const char *message,
                                     common::IoErr io_error) noexcept {
    return ProxyAddressSelectError{
            .code = code,
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
    if (port != 80 && port != 443) {
        result.push_back(':');
        result.append(port_text.data(), converted.ptr);
    }
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

class AccessServiceState::Impl final : public common::NonCopyable, public common::NonMovable {
public:
    using Selection = AccessUpstreamSwrr::Selection;

    Impl(AccessUpstreamSwrr::Options options, std::string zone) :
        options_(std::move(options)), zone_(std::move(zone)) {}

    [[nodiscard]] bool update(const nacos::ServiceInfo &snapshot) noexcept {
        std::vector<AccessEndpointDefinition> next_definitions;
        next_definitions.reserve(snapshot.hosts.size());
        for (const nacos::ServiceInstance &instance: snapshot.hosts) {
            if (!instance.enabled || !instance.healthy || !std::isfinite(instance.weight) || instance.weight <= 0.0 ||
                instance.ip.empty() || instance.port == 0) {
                continue;
            }

            net::IpAddress ip;
            const bool parsed_ip = net::IpAddress::parse(instance.ip, ip);
            const auto scheme = instance.port == 443 ? http::Http1ConnectionGroupKey::Scheme::Https
                                                     : http::Http1ConnectionGroupKey::Scheme::Http;
            std::optional<http::Http1ConnectionGroupKey> connection_key =
                    parsed_ip ? std::optional(http::Http1ConnectionGroupKey::from_ip(ip, instance.port, scheme))
                              : http::Http1ConnectionGroupKey::from_name(instance.ip, instance.port, scheme);
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
            return std::unexpected(SwrrSelectError::NoConfiguredInstance);
        }
        auto preferred = selected_cluster->preferred->select(excluded_selection_tokens);
        if (preferred) {
            return preferred;
        }
        auto fallback = selected_cluster->fallback->select(excluded_selection_tokens);
        if (fallback) {
            return fallback;
        }
        if (preferred.error() == SwrrSelectError::NoAvailableInstance ||
            fallback.error() == SwrrSelectError::NoAvailableInstance) {
            return std::unexpected(SwrrSelectError::NoAvailableInstance);
        }
        return std::unexpected(SwrrSelectError::NoConfiguredInstance);
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

AccessServiceState::AccessServiceState() noexcept = default;

AccessServiceState::~AccessServiceState() noexcept = default;

void AccessServiceState::initialize(AccessUpstreamSwrr::Options options, std::string_view zone) noexcept {
    FIBER_ASSERT(impl_ == nullptr);
    impl_.reset(new (std::nothrow) Impl(std::move(options), std::string(zone)));
    FIBER_ASSERT(impl_ != nullptr);
}

void AccessServiceState::update(const nacos::ServiceInfo &snapshot) noexcept {
    FIBER_ASSERT(impl_ != nullptr);
    (void) impl_->update(snapshot);
}

std::expected<AccessServiceState::Selection, SwrrSelectError>
AccessServiceState::select(std::string_view cluster,
                           std::span<const std::uint64_t> excluded_selection_tokens) noexcept {
    FIBER_ASSERT(impl_ != nullptr);
    return impl_->select(cluster, excluded_selection_tokens);
}

namespace {

class NacosProxyAddressSelector final : public ProxyAddressSelector {
public:
    NacosProxyAddressSelector(AccessServiceDiscovery::Lease lease, std::string cluster) :
        lease_(std::move(lease)), cluster_(std::move(cluster)) {
        FIBER_ASSERT(lease_);
        FIBER_ASSERT(!cluster_.empty());
    }

    [[nodiscard]] async::Task<std::expected<void, ProxyAddressReadyError>> wait_ready() noexcept override {
        auto ready = co_await lease_.wait_ready();
        if (ready) {
            co_return std::expected<void, ProxyAddressReadyError>{};
        }
        const char *message = "service discovery retired before its initial update";
        common::IoErr io_error = common::IoErr::Canceled;
        if (ready.error() == nacos::ServiceReadyError::Closed) {
            message = "service discovery closed before its initial update";
            io_error = common::IoErr::NotConnected;
        } else if (ready.error() == nacos::ServiceReadyError::Shutdown) {
            message = "service discovery shut down before its initial update";
        }
        co_return std::unexpected(ProxyAddressReadyError{.io_error = io_error, .message = message});
    }

    [[nodiscard]] bool ready_for_publish() const noexcept override { return false; }

    std::expected<ProxyUpstreamEndpoint, ProxyAddressSelectError>
    select_address(std::optional<std::string_view> cluster_override,
                   std::span<const std::uint64_t> excluded_selection_tokens) noexcept override {
        std::string_view cluster = cluster_;
        if (cluster_override && !cluster_override->empty()) {
            cluster = *cluster_override;
        }
        auto selected = lease_.state().select(cluster, excluded_selection_tokens);
        if (!selected) {
            const ProxyAddressSelectErrorCode code = selected.error() == SwrrSelectError::NoConfiguredInstance
                                                             ? ProxyAddressSelectErrorCode::NoHosts
                                                             : ProxyAddressSelectErrorCode::CircuitOpen;
            const char *message = code == ProxyAddressSelectErrorCode::NoHosts
                                          ? "no available service instance"
                                          : "service upstream circuit breaker is open";
            return std::unexpected(select_error(code, message, common::IoErr::NotFound));
        }
        return make_proxy_upstream_endpoint(std::move(*selected), lease_.service_name());
    }

    [[nodiscard]] std::string_view service_name() const noexcept override { return lease_.service_name(); }

    [[nodiscard]] std::optional<std::string_view> configured_cluster() const noexcept override { return cluster_; }

private:
    AccessServiceDiscovery::Lease lease_;
    std::string cluster_;
};

} // namespace

void AccessServiceOps::on_init(const nacos::ServiceKeyView &, State &state) noexcept {
    state.initialize(swrr_options, zone);
}

void AccessServiceOps::on_update(const nacos::ServiceKeyView &, State &state,
                                 const std::shared_ptr<const nacos::ServiceInfo> &snapshot) noexcept {
    FIBER_ASSERT(snapshot != nullptr);
    state.update(*snapshot);
}

void AccessServiceOps::on_retire(const nacos::ServiceKeyView &, State &state, nacos::ServiceRetireReason) noexcept {
    state.update(nacos::ServiceInfo{});
}

AccessServiceSelectorFactory::AccessServiceSelectorFactory(AccessServiceDiscovery &discovery,
                                                           AccessServiceDiscoveryOptions options) noexcept :
    discovery_(&discovery), options_(std::move(options)) {}

ProxyAddressSelectorFactory AccessServiceSelectorFactory::adapter() noexcept {
    return ProxyAddressSelectorFactory{
            .context = this,
            .create_service = &AccessServiceSelectorFactory::create_address_selector,
    };
}

std::shared_ptr<ProxyAddressSelector>
AccessServiceSelectorFactory::create_address_selector(void *context, std::string service, std::string cluster) {
    auto &self = *static_cast<AccessServiceSelectorFactory *>(context);
    FIBER_ASSERT(self.discovery_ != nullptr);
    auto acquired = self.discovery_->acquire(service, self.options_.group);
    if (!acquired) {
        if (!self.acquire_error_) {
            self.acquire_error_ = std::move(acquired.error());
        }
        return make_unavailable_service_address_selector(std::move(service), std::move(cluster));
    }
    return std::make_shared<NacosProxyAddressSelector>(std::move(*acquired), std::move(cluster));
}

} // namespace fiber::access_server
