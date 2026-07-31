#include "NacosServiceSelector.h"

#include <algorithm>
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

} // namespace

struct NacosServiceSelector::Directory {
    struct Item {
        std::string service;
        std::shared_ptr<nacos::LoadBalancer> load_balancer;
    };

    [[nodiscard]] std::shared_ptr<nacos::LoadBalancer> find(std::string_view service) const noexcept {
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
    discovery_(loop, naming_service,
               nacos::ServiceDiscoveryOptions{
                       .load_balancer =
                               nacos::LoadBalancer::Options{
                                       .max_fails = 25,
                               },
               },
               nacos::ServiceDiscoveryObserver{
                       .context = this,
                       .on_update = &NacosServiceSelector::service_updated,
               }) {
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
    std::shared_ptr<nacos::LoadBalancer> load_balancer = directory->find(service);
    if (!load_balancer) {
        return std::unexpected(
                select_error("service is not present in the active route directory", common::IoErr::NotFound));
    }

    const std::string_view selected_cluster =
            cluster && !cluster->empty() ? *cluster : std::string_view(options_.default_cluster);
    auto selected = load_balancer->load_balance(nacos::ServiceInstanceSelection{
            .cluster = selected_cluster,
            .preferred_zone = options_.zone,
            .excluded_peer_ids = excluded_selection_tokens,
    });
    if (!selected) {
        return std::unexpected(select_error(selected.error() == nacos::LoadBalanceError::Uninitialized
                                                    ? "service discovery has not received an initial value"
                                                    : "no available service instance",
                                            common::IoErr::NotFound));
    }

    const std::uint64_t selection_token = selected->peer_id();
    return ProxyUpstreamEndpoint{
            .scheme = ProxyUpstreamScheme::Http,
            .host = selected->host(),
            .port = selected->port(),
            .host_header = selected->authority(),
            .ip_address = selected->ip_address(),
            .service_instance = std::move(*selected),
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
    if (endpoint.service_instance.pending()) {
        endpoint.service_instance.report(success ? nacos::InstanceReportOutcome::Success
                                                 : nacos::InstanceReportOutcome::Failure);
    }
}

void NacosServiceSelector::service_updated(void *context, nacos::LoadBalancer &, std::string_view, std::string_view,
                                           bool, nacos::LoadBalancerUpdateResult) {
    auto &self = *static_cast<NacosServiceSelector *>(context);
    ++self.naming_updates_;
}

void NacosServiceSelector::publish_directory() {
    auto directory = std::make_shared<Directory>();
    directory->services.reserve(entries_.size());
    for (const auto &[service, handle]: entries_) {
        directory->services.push_back(Directory::Item{
                .service = service,
                .load_balancer = handle.shared_load_balancer(),
        });
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
