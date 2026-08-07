#include "ProxyAddressSelector.h"

#include <limits>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

ProxyAddressSelectError unavailable_error(ProxyAddressSelectErrorCode code, const char *message) noexcept {
    return ProxyAddressSelectError{
            .code = code,
            .io_error = common::IoErr::NotFound,
            .message = message,
    };
}

class StaticProxyAddressSelector final : public ProxyAddressSelector {
public:
    explicit StaticProxyAddressSelector(std::vector<AccessUpstreamInstance> addresses) {
        std::vector<AccessUpstreamSwrr::WeightedInstance> weighted;
        weighted.reserve(addresses.size());
        std::uint64_t selection_token = 0;
        for (AccessUpstreamInstance &address: addresses) {
            FIBER_ASSERT(selection_token != std::numeric_limits<std::uint64_t>::max());
            weighted.push_back(AccessUpstreamSwrr::WeightedInstance{
                    .selection_token = ++selection_token,
                    .instance = std::move(address),
                    .weight = 1.0,
            });
        }
        (void) swrr_.update(std::move(weighted));
    }

    std::expected<ProxyUpstreamEndpoint, ProxyAddressSelectError>
    select_address(std::optional<std::string_view>,
                   std::span<const std::uint64_t> excluded_selection_tokens) noexcept override {
        auto selected = swrr_.select(excluded_selection_tokens);
        if (!selected) {
            const ProxyAddressSelectErrorCode code = selected.error() == SwrrSelectError::NoConfiguredInstance
                                                             ? ProxyAddressSelectErrorCode::NoHosts
                                                             : ProxyAddressSelectErrorCode::CircuitOpen;
            const char *message = code == ProxyAddressSelectErrorCode::NoHosts
                                          ? "no available static upstream address"
                                          : "static upstream circuit breaker is open";
            return std::unexpected(unavailable_error(code, message));
        }
        const std::string_view provider_name = selected->instance().authority;
        return make_proxy_upstream_endpoint(std::move(*selected), provider_name);
    }

private:
    AccessUpstreamSwrr swrr_;
};

class UnavailableServiceAddressSelector final : public ProxyAddressSelector {
public:
    UnavailableServiceAddressSelector(std::string service, std::string cluster) :
        service_(std::move(service)), cluster_(std::move(cluster)) {
        FIBER_ASSERT(!cluster_.empty());
    }

    std::expected<ProxyUpstreamEndpoint, ProxyAddressSelectError>
    select_address(std::optional<std::string_view>, std::span<const std::uint64_t>) noexcept override {
        return std::unexpected(
                unavailable_error(ProxyAddressSelectErrorCode::NoHosts, "service upstream selector is unavailable"));
    }

    [[nodiscard]] std::string_view service_name() const noexcept override { return service_; }

    [[nodiscard]] std::optional<std::string_view> configured_cluster() const noexcept override { return cluster_; }

private:
    std::string service_;
    std::string cluster_;
};

} // namespace

std::shared_ptr<ProxyAddressSelector>
make_static_proxy_address_selector(std::vector<AccessUpstreamInstance> addresses) {
    return std::make_shared<StaticProxyAddressSelector>(std::move(addresses));
}

std::shared_ptr<ProxyAddressSelector> make_unavailable_service_address_selector(std::string service,
                                                                                std::string cluster) {
    return std::make_shared<UnavailableServiceAddressSelector>(std::move(service), std::move(cluster));
}

} // namespace fiber::access_server
