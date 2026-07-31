#include "AccessRouteSnapshot.h"

#include <string>

namespace fiber::access_server {
namespace {

AccessConfigError snapshot_error(std::string_view field, std::string_view message) {
    return AccessConfigError{
            .code = AccessConfigErrorCode::Conflict,
            .field = std::string(field),
            .message = std::string(message),
    };
}

} // namespace

std::expected<AccessRouteSnapshot, AccessConfigError>
AccessRouteSnapshot::build(std::span<const std::shared_ptr<const ProjectRouteSnapshot>> projects) {
    AccessRouteSnapshot snapshot;
    snapshot.projects_.reserve(projects.size());

    std::size_t host_count = 0;
    for (const std::shared_ptr<const ProjectRouteSnapshot> &project: projects) {
        if (!project) {
            return std::unexpected(snapshot_error("projects", "project snapshot is null"));
        }
        for (const std::shared_ptr<const ProjectRouteSnapshot> &existing: snapshot.projects_) {
            if (existing->project() == project->project()) {
                return std::unexpected(snapshot_error("projects", "project name is duplicate"));
            }
        }
        host_count += project->hosts().size();
        snapshot.projects_.push_back(project);
    }

    std::vector<HostPattern> patterns;
    patterns.reserve(host_count);
    snapshot.host_targets_.reserve(host_count);
    for (std::uint32_t project_index = 0; project_index < snapshot.projects_.size(); ++project_index) {
        const auto &hosts = snapshot.projects_[project_index]->hosts();
        for (std::uint32_t host_index = 0; host_index < hosts.size(); ++host_index) {
            const std::uint32_t target = static_cast<std::uint32_t>(snapshot.host_targets_.size());
            snapshot.host_targets_.push_back(HostTarget{
                    .project_index = project_index,
                    .host_index = host_index,
            });
            patterns.push_back(HostPattern{
                    .pattern = hosts[host_index].pattern,
                    .handler = target,
            });
        }
    }

    auto matcher = HostMatcher::build(patterns);
    if (!matcher) {
        return std::unexpected(std::move(matcher.error()));
    }
    snapshot.host_matcher_ = std::move(*matcher);
    return snapshot;
}

ProjectHostMatch AccessRouteSnapshot::match_host(std::string_view host) const noexcept {
    const std::optional<std::uint32_t> target_index = host_matcher_.match(host);
    if (!target_index) {
        return {};
    }
    const HostTarget &target = host_targets_[*target_index];
    const ProjectRouteSnapshot *project = projects_[target.project_index].get();
    return ProjectHostMatch{
            .project = project,
            .host = &project->hosts()[target.host_index],
    };
}

} // namespace fiber::access_server
