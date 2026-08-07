#ifndef FIBER_ACCESS_SERVER_ACCESS_SERVICE_DISCOVERY_H
#define FIBER_ACCESS_SERVER_ACCESS_SERVICE_DISCOVERY_H

#include "../config/AccessConfig.h"
#include "../routing/ProxyAddressSelector.h"
#include "SmoothWeightedRoundRobin.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/nacos/discovery/ServiceDiscovery.h>

namespace fiber::access_server {

class AccessServiceState final : public common::NonCopyable, public common::NonMovable {
public:
    using Selection = AccessUpstreamSwrr::Selection;

    AccessServiceState() noexcept;
    ~AccessServiceState() noexcept;

    void initialize(AccessUpstreamSwrr::Options options, std::string_view zone) noexcept;
    void update(const nacos::ServiceInfo &snapshot) noexcept;

    [[nodiscard]] std::expected<Selection, SwrrSelectError>
    select(std::string_view cluster, std::span<const std::uint64_t> excluded_selection_tokens) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct AccessServiceOps {
    using State = AccessServiceState;

    void on_init(const nacos::ServiceKeyView &key, State &state) noexcept;
    void on_update(const nacos::ServiceKeyView &key, State &state,
                   const std::shared_ptr<const nacos::ServiceInfo> &snapshot) noexcept;
    void on_retire(const nacos::ServiceKeyView &key, State &state, nacos::ServiceRetireReason reason) noexcept;

    AccessUpstreamSwrr::Options swrr_options{};
    std::string zone;
};

using AccessServiceDiscovery = nacos::ServiceDiscovery<AccessServiceOps>;

struct AccessServiceDiscoveryOptions {
    std::string group = std::string(kDefaultNacosGroup);
    std::string zone;
    AccessUpstreamSwrr::Options swrr_options{};
};

// Synchronous adapter used only while compiling one project snapshot. Each
// service route acquires its own Lease; ServiceDiscovery performs key-level
// subscription deduplication.
class AccessServiceSelectorFactory final : public common::NonCopyable, public common::NonMovable {
public:
    AccessServiceSelectorFactory() noexcept = default;
    AccessServiceSelectorFactory(AccessServiceDiscovery &discovery, AccessServiceDiscoveryOptions options) noexcept;

    [[nodiscard]] ProxyAddressSelectorFactory adapter() noexcept;
    void begin_compile() noexcept { acquire_error_.reset(); }
    [[nodiscard]] std::optional<nacos::NamingServiceError> take_error() noexcept {
        return std::exchange(acquire_error_, std::nullopt);
    }

private:
    [[nodiscard]] static std::shared_ptr<ProxyAddressSelector>
    create_address_selector(void *context, std::string service, std::string cluster);

    AccessServiceDiscovery *discovery_ = nullptr;
    AccessServiceDiscoveryOptions options_;
    std::optional<nacos::NamingServiceError> acquire_error_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SERVICE_DISCOVERY_H
