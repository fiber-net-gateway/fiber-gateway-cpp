#ifndef FIBER_ACCESS_SERVER_PROXY_ADDRESS_SELECTOR_H
#define FIBER_ACCESS_SERVER_PROXY_ADDRESS_SELECTOR_H

#include <fiber/http/Http1ConnectionGroupKey.h>
#include "../runtime/SmoothWeightedRoundRobin.h"

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::access_server {

enum class ProxyAddressSelectErrorCode : std::uint8_t {
    NoHosts,
    CircuitOpen,
};

struct ProxyAddressSelectError {
    ProxyAddressSelectErrorCode code = ProxyAddressSelectErrorCode::NoHosts;
    common::IoErr io_error = common::IoErr::None;
    const char *message = nullptr;
};

struct ProxyAddressReadyError {
    common::IoErr io_error = common::IoErr::None;
    const char *message = nullptr;
};

struct AccessUpstreamInstance {
    http::Http1ConnectionGroupKey connection_key;
    std::string authority;

    friend bool operator==(const AccessUpstreamInstance &, const AccessUpstreamInstance &) noexcept = default;
};

using AccessUpstreamSwrr = SmoothWeightedRoundRobin<AccessUpstreamInstance>;

// The SWRR selection pins the selected address generation for as long as
// connection_key and host_header are consumed by the request.
struct ProxyUpstreamEndpoint {
    const http::Http1ConnectionGroupKey *connection_key = nullptr;
    std::string_view host_header;
    std::string_view provider_name;
    AccessUpstreamSwrr::Selection selection;
    std::uint64_t selection_token = 0;

    void report(bool success) noexcept {
        if (selection.pending()) {
            selection.report(success);
        }
    }
};

[[nodiscard]] inline ProxyUpstreamEndpoint make_proxy_upstream_endpoint(AccessUpstreamSwrr::Selection selection,
                                                                        std::string_view provider_name) noexcept {
    const std::uint64_t selection_token = selection.selection_token();
    const AccessUpstreamInstance &instance = selection.instance();
    return ProxyUpstreamEndpoint{
            .connection_key = &instance.connection_key,
            .host_header = instance.authority,
            .provider_name = provider_name,
            .selection = std::move(selection),
            .selection_token = selection_token,
    };
}

class ProxyAddressSelector {
public:
    virtual ~ProxyAddressSelector() = default;

    [[nodiscard]] virtual async::Task<std::expected<void, ProxyAddressReadyError>> wait_ready() noexcept {
        co_return std::expected<void, ProxyAddressReadyError>{};
    }
    [[nodiscard]] virtual bool ready_for_publish() const noexcept { return true; }

    [[nodiscard]] virtual std::expected<ProxyUpstreamEndpoint, ProxyAddressSelectError>
    select_address(std::optional<std::string_view> cluster_override,
                   std::span<const std::uint64_t> excluded_selection_tokens) noexcept = 0;

    virtual void report_address(ProxyUpstreamEndpoint &endpoint, bool success) noexcept { endpoint.report(success); }

    // Empty for selectors which do not require service discovery. This is
    // control-plane metadata used to reconcile Nacos subscriptions.
    [[nodiscard]] virtual std::string_view service_name() const noexcept { return {}; }
    [[nodiscard]] virtual std::optional<std::string_view> configured_cluster() const noexcept { return std::nullopt; }
};

struct ProxyAddressSelectorFactory {
    using Function = std::shared_ptr<ProxyAddressSelector> (*)(void *context, std::string service, std::string cluster);

    void *context = nullptr;
    Function create_service = nullptr;
};

struct ProxyClusterMatcher {
    using Function = bool (*)(void *context, const http::HttpExchange &exchange) noexcept;

    void *context = nullptr;
    Function matches = nullptr;
};

[[nodiscard]] std::shared_ptr<ProxyAddressSelector>
make_static_proxy_address_selector(std::vector<AccessUpstreamInstance> addresses);

[[nodiscard]] std::shared_ptr<ProxyAddressSelector> make_unavailable_service_address_selector(std::string service,
                                                                                              std::string cluster);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROXY_ADDRESS_SELECTOR_H
