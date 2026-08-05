#ifndef FIBER_ACCESS_SERVER_ROUTE_CONFIG_STORE_H
#define FIBER_ACCESS_SERVER_ROUTE_CONFIG_STORE_H

#include "../routing/AccessRouteSnapshot.h"
#include "AccessServiceDiscovery.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fiber::access_server {

enum class ConfigUpdateStatus : std::uint8_t {
    IgnoredEmpty,
    VersionUnchanged,
    Published,
    Unloaded,
    ProjectRemoved,
};

struct ConfigUpdateResult {
    ConfigUpdateStatus status = ConfigUpdateStatus::IgnoredEmpty;
    std::shared_ptr<const AccessRouteSnapshot> snapshot;
};

using ConfigUpdateOutcome = std::expected<ConfigUpdateResult, AccessConfigError>;

struct PreparedConfigUpdate {
    ConfigUpdateStatus status = ConfigUpdateStatus::IgnoredEmpty;
    std::string project;
    std::int32_t version = 0;
    std::shared_ptr<const ProjectRouteSnapshot> project_snapshot;

    [[nodiscard]] bool needs_publish() const noexcept {
        return status == ConfigUpdateStatus::Published || status == ConfigUpdateStatus::Unloaded;
    }
};

using PreparedConfigUpdateOutcome = std::expected<PreparedConfigUpdate, AccessConfigError>;

// Mutation is serialized by the runtime owner EventLoop. Requests may pin the
// immutable published snapshot from any serving thread.
class RouteConfigStore {
public:
    explicit RouteConfigStore(ScriptCompilerAdapter script_compiler = {},
                              ProxyAddressSelectorFactory selector_factory = {});
    RouteConfigStore(ScriptCompilerAdapter script_compiler, AccessServiceDiscovery &service_discovery,
                     AccessServiceDiscoveryOptions discovery_options = {});

    [[nodiscard]] PreparedConfigUpdateOutcome prepare(std::string_view project,
                                                      const std::optional<ProjectConfig> &config);
    [[nodiscard]] ConfigUpdateOutcome apply(std::string_view project, const std::optional<ProjectConfig> &config);
    [[nodiscard]] ConfigUpdateOutcome commit(PreparedConfigUpdate prepared);
    [[nodiscard]] ConfigUpdateOutcome remove_project(std::string_view project);
    void clear() noexcept;

    [[nodiscard]] std::shared_ptr<const AccessRouteSnapshot> pin() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        return published_.load(std::memory_order_acquire);
#else
        return std::atomic_load_explicit(&published_, std::memory_order_acquire);
#endif
    }

private:
    struct PublishedVersion {
        std::string project;
        std::int32_t version = 0;
    };

    [[nodiscard]] std::optional<std::int32_t> published_version(std::string_view project) const noexcept;
    void set_published_version(std::string_view project, std::int32_t version);
    void remove_published_version(std::string_view project);
    [[nodiscard]] ConfigUpdateOutcome
    publish_candidate(std::vector<std::shared_ptr<const ProjectRouteSnapshot>> candidate, ConfigUpdateStatus status);

    std::vector<std::shared_ptr<const ProjectRouteSnapshot>> projects_;
    std::vector<PublishedVersion> published_versions_;
    ScriptCompilerAdapter script_compiler_;
    AccessServiceSelectorFactory service_selector_factory_;
    ProxyAddressSelectorFactory selector_factory_;
    bool uses_service_discovery_ = false;
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const AccessRouteSnapshot>> published_;
#else
    std::shared_ptr<const AccessRouteSnapshot> published_;
#endif
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ROUTE_CONFIG_STORE_H
