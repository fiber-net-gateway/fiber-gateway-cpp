#ifndef FIBER_ACCESS_SERVER_ACCESS_ROUTE_SNAPSHOT_H
#define FIBER_ACCESS_SERVER_ACCESS_ROUTE_SNAPSHOT_H

#include "ProjectRouteSnapshot.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace fiber::access_server {

struct ProjectHostMatch {
    const ProjectRouteSnapshot *project = nullptr;
    const CompiledHost *host = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept { return project != nullptr; }
};

class AccessRouteSnapshot {
public:
    AccessRouteSnapshot() = default;

    [[nodiscard]] static std::expected<AccessRouteSnapshot, AccessConfigError>
    build(std::span<const std::shared_ptr<const ProjectRouteSnapshot>> projects);

    [[nodiscard]] ProjectHostMatch match_host(std::string_view host) const noexcept;
    [[nodiscard]] const std::vector<std::shared_ptr<const ProjectRouteSnapshot>> &projects() const noexcept {
        return projects_;
    }

private:
    struct HostTarget {
        std::uint32_t project_index = 0;
        std::uint32_t host_index = 0;
    };

    std::vector<std::shared_ptr<const ProjectRouteSnapshot>> projects_;
    std::vector<HostTarget> host_targets_;
    HostMatcher host_matcher_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_ROUTE_SNAPSHOT_H
