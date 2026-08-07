#include "AccessConfigWatcher.h"

#include "../config/AccessConfigCodec.h"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/TaskSelect.h>
#include <fiber/async/WhenAny.h>
#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

template<typename Entry>
void request_stop(Entry &entry) noexcept {
    if (!entry.stopping) {
        entry.stopping = true;
        entry.subscription.close();
    }
}

} // namespace

struct AccessConfigWatcher::ProjectListEntry final : public common::NonCopyable, public common::NonMovable {
    explicit ProjectListEntry(AccessConfigWatcher &value_owner) : owner(&value_owner) {}

    AccessConfigWatcher *owner = nullptr;
    nacos::Subscription<nacos::ConfigData> subscription;
    bool stopping = false;
};

struct AccessConfigWatcher::ProjectEntry final : public common::NonCopyable, public common::NonMovable {
    ProjectEntry(AccessConfigWatcher &value_owner, std::string value_project) :
        owner(&value_owner), project(std::move(value_project)), revisions(0),
        revision_publisher(revisions.acquire_publisher()) {
        FIBER_ASSERT(revision_publisher.has_value());
    }

    void advance() noexcept {
        FIBER_ASSERT(generation != std::numeric_limits<std::uint64_t>::max());
        revision_publisher->publish(++generation);
    }

    AccessConfigWatcher *owner = nullptr;
    std::string project;
    nacos::Subscription<nacos::ConfigData> subscription;
    async::Watch<std::uint64_t> revisions;
    std::optional<async::Watch<std::uint64_t>::Publisher> revision_publisher;
    std::uint64_t generation = 0;
    bool stopping = false;
};

AccessConfigWatcher::AccessConfigWatcher(event::EventLoop &loop, nacos::ConfigService &config_service,
                                         RouteConfigStore &store, AccessConfigWatcherOptions options,
                                         RouteSnapshotObserver observer) :
    loop_(&loop), config_service_(&config_service), store_(&store), options_(std::move(options)), observer_(observer) {
    ready_publisher_ = ready_.acquire_publisher();
    FIBER_ASSERT(ready_publisher_.has_value());
}

AccessConfigWatcher::~AccessConfigWatcher() {
    FIBER_ASSERT(state_ == AccessConfigWatcherState::Created || state_ == AccessConfigWatcherState::Stopped);
    FIBER_ASSERT(project_list_ == nullptr);
    FIBER_ASSERT(projects_.empty());
}

std::expected<void, nacos::ConfigServiceError> AccessConfigWatcher::start() {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != AccessConfigWatcherState::Created) {
        return std::unexpected(nacos::ConfigServiceError{
                .code = nacos::ConfigServiceErrorCode::InvalidArgument,
                .io_error = common::IoErr::Already,
                .message = "access config watcher is already started",
        });
    }

    state_ = AccessConfigWatcherState::Running;
    project_list_ = std::make_unique<ProjectListEntry>(*this);
    auto subscription = config_service_->subscribe(options_.project_list_data_id, options_.project_route_group,
                                                   &project_list_notify, project_list_.get());
    if (!subscription) {
        project_list_.reset();
        state_ = AccessConfigWatcherState::Created;
        return std::unexpected(std::move(subscription.error()));
    }
    project_list_->subscription = std::move(*subscription);
    return {};
}

async::Task<void> AccessConfigWatcher::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == AccessConfigWatcherState::Stopped) {
        co_return;
    }
    if (state_ == AccessConfigWatcherState::Created) {
        state_ = AccessConfigWatcherState::Stopped;
        co_return;
    }
    if (state_ == AccessConfigWatcherState::Running) {
        state_ = AccessConfigWatcherState::Stopping;
    }
    request_stop(*project_list_);
    for (auto &[project, entry]: projects_) {
        (void) project;
        entry->advance();
        request_stop(*entry);
    }
    co_await readiness_tasks_.join();
    projects_.clear();
    project_list_.reset();
    state_ = AccessConfigWatcherState::Stopped;
}

void AccessConfigWatcher::project_list_notify(void *context,
                                              const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto &entry = *static_cast<ProjectListEntry *>(context);
    AccessConfigWatcher &owner = *entry.owner;
    if (result.kind == nacos::ResultKind::Closed) {
        request_stop(entry);
        return;
    }
    if (result.data && owner.state_ == AccessConfigWatcherState::Running) {
        owner.apply_project_list(*result.data);
    }
}

void AccessConfigWatcher::project_notify(void *context,
                                         const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto *entry = static_cast<ProjectEntry *>(context);
    AccessConfigWatcher &owner = *entry->owner;
    const auto found = owner.projects_.find(entry->project);
    if (found == owner.projects_.end() || found->second.get() != entry) {
        return;
    }
    std::shared_ptr<ProjectEntry> hold = found->second;
    if (result.kind == nacos::ResultKind::Closed) {
        request_stop(*hold);
        return;
    }
    if (result.data && !hold->stopping && owner.state_ == AccessConfigWatcherState::Running) {
        owner.apply_project(hold, *result.data);
    }
}

void AccessConfigWatcher::apply_project_list(const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    if (data.state == nacos::ConfigState::NotFound) {
        reconcile_projects({});
    } else {
        reconcile_projects(data.content);
    }
    if (!initial_project_list_received_) {
        initial_project_list_received_ = true;
        ready_publisher_->publish(true);
    }
}

void AccessConfigWatcher::apply_project(const std::shared_ptr<ProjectEntry> &entry, const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    entry->advance();
    if (data.state == nacos::ConfigState::NotFound || data.content.empty()) {
        auto ignored = store_->prepare(entry->project, std::nullopt);
        FIBER_ASSERT(ignored.has_value());
        return;
    }

    auto parsed = parse_project_config(data.content);
    if (!parsed) {
        report_failure(options_.project_route_data_id_prefix + entry->project, std::string(data.md5),
                       std::move(parsed.error()));
        return;
    }
    auto prepared = store_->prepare(entry->project, *parsed);
    if (!prepared) {
        report_failure(options_.project_route_data_id_prefix + entry->project, std::string(data.md5),
                       std::move(prepared.error()));
        return;
    }

    if (!prepared->needs_publish()) {
        return;
    }
    if (prepared->status == ConfigUpdateStatus::Unloaded) {
        auto updated = store_->commit(std::move(*prepared));
        if (!updated) {
            report_failure(options_.project_route_data_id_prefix + entry->project, std::string(data.md5),
                           std::move(updated.error()));
            return;
        }
        ++successful_updates_;
        publish_observer(updated->snapshot);
        return;
    }

    const std::uint64_t generation = entry->generation;
    const std::uint64_t revision_version = entry->revisions.current().version;
    std::string data_id = options_.project_route_data_id_prefix + entry->project;
    readiness_tasks_.add();
    async::spawn([this, entry, prepared = std::move(*prepared), generation, revision_version,
                  data_id = std::move(data_id), md5 = std::string(data.md5)]() mutable {
        return apply_ready_project(std::move(entry), std::move(prepared), generation, revision_version,
                                   std::move(data_id), std::move(md5));
    });
}

async::DetachedTask AccessConfigWatcher::apply_ready_project(std::shared_ptr<ProjectEntry> entry,
                                                             PreparedConfigUpdate prepared, std::uint64_t generation,
                                                             std::uint64_t revision_version, std::string data_id,
                                                             std::string md5) noexcept {
    auto revisions = entry->revisions.subscribe();
    auto ready_or_replaced =
            co_await async::when_any([&prepared]() { return prepared.project_snapshot->wait_ready().select(); },
                                     [&revisions, revision_version]() { return revisions.next(revision_version); });

    if (ready_or_replaced.is<0>()) {
        auto ready = std::move(ready_or_replaced).get<0>();
        const auto found = projects_.find(entry->project);
        const bool current = state_ == AccessConfigWatcherState::Running && found != projects_.end() &&
                             found->second.get() == entry.get() && entry->generation == generation;
        if (current && !ready) {
            report_failure(std::move(data_id), std::move(md5),
                           AccessConfigError{
                                   .code = AccessConfigErrorCode::InvalidCombination,
                                   .field = "service",
                                   .message = ready.error().message,
                           });
        } else if (current) {
            auto updated = store_->commit(std::move(prepared));
            if (!updated) {
                report_failure(std::move(data_id), std::move(md5), std::move(updated.error()));
            } else {
                ++successful_updates_;
                publish_observer(updated->snapshot);
            }
        }
    } else {
        std::move(ready_or_replaced).get<1>();
    }
    readiness_tasks_.done();
}

void AccessConfigWatcher::reconcile_projects(std::string_view content) {
    FIBER_ASSERT(loop_->in_loop());
    std::vector<std::string> requested = parse_project_list(content);
    std::set<std::string, std::less<>> unique;
    for (std::string &project: requested) {
        if (unique.emplace(project).second && !projects_.contains(project)) {
            add_project(std::move(project));
        }
    }

    std::vector<std::string> removed;
    removed.reserve(projects_.size());
    for (const auto &[project, entry]: projects_) {
        (void) entry;
        if (!unique.contains(project)) {
            removed.push_back(project);
        }
    }
    for (const std::string &project: removed) {
        remove_project(project);
    }
}

void AccessConfigWatcher::add_project(std::string project) {
    FIBER_ASSERT(loop_->in_loop());
    std::string data_id = options_.project_route_data_id_prefix;
    data_id.append(project);
    auto entry = std::make_shared<ProjectEntry>(*this, std::move(project));
    auto [iterator, inserted] = projects_.emplace(entry->project, entry);
    FIBER_ASSERT(inserted);
    auto subscription = config_service_->subscribe(data_id, options_.project_route_group, &project_notify, entry.get());
    if (!subscription) {
        projects_.erase(iterator);
        ++failed_updates_;
        return;
    }
    entry->subscription = std::move(*subscription);
}

void AccessConfigWatcher::remove_project(std::string_view project) {
    FIBER_ASSERT(loop_->in_loop());
    const auto iterator = projects_.find(project);
    if (iterator == projects_.end()) {
        return;
    }
    std::shared_ptr<ProjectEntry> retiring = std::move(iterator->second);
    projects_.erase(iterator);
    retiring->advance();
    request_stop(*retiring);

    auto removed = store_->remove_project(project);
    FIBER_ASSERT(removed.has_value());
    ++successful_updates_;
    publish_observer(removed->snapshot);
}

void AccessConfigWatcher::publish_observer(const std::shared_ptr<const AccessRouteSnapshot> &snapshot) const noexcept {
    if (observer_.on_update) {
        observer_.on_update(observer_.context, snapshot);
    }
}

void AccessConfigWatcher::report_failure(std::string data_id, std::string md5, AccessConfigError error) {
    ++failed_updates_;
    last_failure_ = AccessConfigWatcherFailure{
            .data_id = std::move(data_id),
            .md5 = std::move(md5),
            .error = std::move(error),
    };
}

} // namespace fiber::access_server
