#include "RouteConfigStore.h"

#include <cstddef>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {

RouteConfigStore::RouteConfigStore(ScriptCompilerAdapter script_compiler,
                                   ProxyAddressSelectorFactory selector_factory) :
    script_compiler_(script_compiler), selector_factory_(selector_factory) {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::make_shared<const AccessRouteSnapshot>(), std::memory_order_relaxed);
#else
    std::atomic_store_explicit(&published_, std::make_shared<const AccessRouteSnapshot>(), std::memory_order_relaxed);
#endif
}

RouteConfigStore::RouteConfigStore(ScriptCompilerAdapter script_compiler, AccessServiceDiscovery &service_discovery,
                                   AccessServiceDiscoveryOptions discovery_options) :
    script_compiler_(script_compiler), service_selector_factory_(service_discovery, std::move(discovery_options)),
    selector_factory_(service_selector_factory_.adapter()), uses_service_discovery_(true) {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::make_shared<const AccessRouteSnapshot>(), std::memory_order_relaxed);
#else
    std::atomic_store_explicit(&published_, std::make_shared<const AccessRouteSnapshot>(), std::memory_order_relaxed);
#endif
}

PreparedConfigUpdateOutcome RouteConfigStore::prepare(std::string_view project,
                                                      const std::optional<ProjectConfig> &config) {
    if (!config) {
        return PreparedConfigUpdate{
                .status = ConfigUpdateStatus::IgnoredEmpty,
                .project = std::string(project),
        };
    }

    const std::optional<std::int32_t> current_version = published_version(project);
    if (current_version && *current_version == config->version) {
        return PreparedConfigUpdate{
                .status = ConfigUpdateStatus::VersionUnchanged,
                .project = std::string(project),
                .version = config->version,
        };
    }

    if (uses_service_discovery_) {
        service_selector_factory_.begin_compile();
    }
    auto compiled = compile_project_config(project, *config, script_compiler_, selector_factory_);
    std::optional<nacos::NamingServiceError> acquire_error;
    if (uses_service_discovery_) {
        acquire_error = service_selector_factory_.take_error();
    }
    if (!compiled) {
        return std::unexpected(std::move(compiled.error()));
    }
    if (acquire_error) {
        return std::unexpected(AccessConfigError{
                .code = AccessConfigErrorCode::InvalidCombination,
                .field = "service",
                .message = std::move(acquire_error->message),
        });
    }

    if (!*compiled) {
        return PreparedConfigUpdate{
                .status = ConfigUpdateStatus::Unloaded,
                .project = std::string(project),
                .version = config->version,
        };
    }

    return PreparedConfigUpdate{
            .status = ConfigUpdateStatus::Published,
            .project = std::string(project),
            .version = config->version,
            .project_snapshot = std::make_shared<const ProjectRouteSnapshot>(std::move(**compiled)),
    };
}

ConfigUpdateOutcome RouteConfigStore::apply(std::string_view project, const std::optional<ProjectConfig> &config) {
    auto prepared = prepare(project, config);
    if (!prepared) {
        return std::unexpected(std::move(prepared.error()));
    }
    if (prepared->project_snapshot && !prepared->project_snapshot->ready_for_publish()) {
        return std::unexpected(AccessConfigError{
                .code = AccessConfigErrorCode::InvalidCombination,
                .field = "service",
                .message = "service routes must complete wait_ready before publication",
        });
    }
    return commit(std::move(*prepared));
}

ConfigUpdateOutcome RouteConfigStore::commit(PreparedConfigUpdate prepared) {
    if (!prepared.needs_publish()) {
        return ConfigUpdateResult{
                .status = prepared.status,
                .snapshot = pin(),
        };
    }

    std::vector<std::shared_ptr<const ProjectRouteSnapshot>> candidate = projects_;
    std::size_t existing = candidate.size();
    for (std::size_t i = 0; i < candidate.size(); ++i) {
        if (candidate[i]->project() == prepared.project) {
            existing = i;
            break;
        }
    }

    if (prepared.status == ConfigUpdateStatus::Unloaded) {
        if (existing != candidate.size()) {
            candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(existing));
        }
        // Java ListenerWrap leaves its last successful non-empty ProjectConf
        // version unchanged when an empty Host map unloads a project.
        return publish_candidate(std::move(candidate), ConfigUpdateStatus::Unloaded);
    }

    FIBER_ASSERT(prepared.project_snapshot != nullptr);
    if (existing == candidate.size()) {
        candidate.push_back(std::move(prepared.project_snapshot));
    } else {
        candidate[existing] = std::move(prepared.project_snapshot);
    }

    auto published = publish_candidate(std::move(candidate), ConfigUpdateStatus::Published);
    if (published) {
        set_published_version(prepared.project, prepared.version);
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

void RouteConfigStore::clear() noexcept {
    projects_.clear();
    published_versions_.clear();
    auto empty = std::make_shared<const AccessRouteSnapshot>();
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    published_.store(std::move(empty), std::memory_order_release);
#else
    std::atomic_store_explicit(&published_, std::move(empty), std::memory_order_release);
#endif
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
