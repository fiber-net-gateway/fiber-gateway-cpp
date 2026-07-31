#include "RouteConfigStore.h"

#include <cstddef>
#include <utility>

namespace fiber::access_server {

RouteConfigStore::RouteConfigStore(ScriptCompilerAdapter script_compiler) : script_compiler_(script_compiler) {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::make_shared<const AccessRouteSnapshot>(), std::memory_order_relaxed);
#else
    std::atomic_store_explicit(&published_, std::make_shared<const AccessRouteSnapshot>(), std::memory_order_relaxed);
#endif
}

ConfigUpdateOutcome RouteConfigStore::apply(std::string_view project, const std::optional<ProjectConfig> &config) {
    if (!config) {
        return ConfigUpdateResult{
                .status = ConfigUpdateStatus::IgnoredEmpty,
                .snapshot = pin(),
        };
    }

    const std::optional<std::int32_t> current_version = published_version(project);
    if (current_version && *current_version == config->version) {
        return ConfigUpdateResult{
                .status = ConfigUpdateStatus::VersionUnchanged,
                .snapshot = pin(),
        };
    }

    auto compiled = compile_project_config(project, *config, script_compiler_);
    if (!compiled) {
        return std::unexpected(std::move(compiled.error()));
    }

    std::vector<std::shared_ptr<const ProjectRouteSnapshot>> candidate = projects_;
    std::size_t existing = candidate.size();
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        if (candidate[i]->project() == project) {
            existing = i;
            break;
        }
    }

    if (!*compiled) {
        if (existing != candidate.size()) {
            candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(existing));
        }
        // Java ListenerWrap leaves its last successful non-empty ProjectConf
        // version unchanged when an empty Host map unloads a project.
        return publish_candidate(std::move(candidate), ConfigUpdateStatus::Unloaded);
    }

    auto project_snapshot = std::make_shared<const ProjectRouteSnapshot>(std::move(**compiled));
    if (existing == candidate.size()) {
        candidate.push_back(std::move(project_snapshot));
    } else {
        candidate[existing] = std::move(project_snapshot);
    }

    auto published = publish_candidate(std::move(candidate), ConfigUpdateStatus::Published);
    if (published) {
        set_published_version(project, config->version);
    }
    return published;
}

ConfigUpdateOutcome RouteConfigStore::remove_project(std::string_view project) {
    std::vector<std::shared_ptr<const ProjectRouteSnapshot>> candidate = projects_;
    for (auto iterator = candidate.begin(); iterator != candidate.end(); ++iterator) {
        if ((*iterator)->project() == project) {
            candidate.erase(iterator);
            break;
        }
    }

    auto published = publish_candidate(std::move(candidate), ConfigUpdateStatus::ProjectRemoved);
    if (published) {
        remove_published_version(project);
    }
    return published;
}

std::optional<std::int32_t> RouteConfigStore::published_version(std::string_view project) const noexcept {
    for (const PublishedVersion &entry: published_versions_) {
        if (entry.project == project) {
            return entry.version;
        }
    }
    return std::nullopt;
}

void RouteConfigStore::set_published_version(std::string_view project, std::int32_t version) {
    for (PublishedVersion &entry: published_versions_) {
        if (entry.project == project) {
            entry.version = version;
            return;
        }
    }
    published_versions_.push_back(PublishedVersion{
            .project = std::string(project),
            .version = version,
    });
}

void RouteConfigStore::remove_published_version(std::string_view project) {
    for (auto iterator = published_versions_.begin(); iterator != published_versions_.end(); ++iterator) {
        if (iterator->project == project) {
            published_versions_.erase(iterator);
            return;
        }
    }
}

ConfigUpdateOutcome
RouteConfigStore::publish_candidate(std::vector<std::shared_ptr<const ProjectRouteSnapshot>> candidate,
                                    ConfigUpdateStatus status) {
    auto snapshot = AccessRouteSnapshot::build(candidate);
    if (!snapshot) {
        return std::unexpected(std::move(snapshot.error()));
    }

    auto published = std::make_shared<const AccessRouteSnapshot>(std::move(*snapshot));
    projects_ = std::move(candidate);
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(published, std::memory_order_release);
#else
    std::atomic_store_explicit(&published_, published, std::memory_order_release);
#endif
    return ConfigUpdateResult{
            .status = status,
            .snapshot = std::move(published),
    };
}

} // namespace fiber::access_server
