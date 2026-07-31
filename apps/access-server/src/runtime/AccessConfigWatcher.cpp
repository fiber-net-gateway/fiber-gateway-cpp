#include "AccessConfigWatcher.h"

#include "../config/AccessConfigCodec.h"

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include <async/Spawn.h>
#include <async/Watch.h>
#include <async/WhenAny.h>
#include <common/Assert.h>

namespace fiber::access_server {
namespace {

template<typename Entry>
void request_stop(Entry &entry) noexcept {
    if (!entry.stopping) {
        entry.stopping = true;
        entry.stop_publisher->publish(true);
    }
}

} // namespace

struct AccessConfigWatcher::ProjectListEntry final : public common::NonCopyable, public common::NonMovable {
    explicit ProjectListEntry(nacos::Subscription<nacos::ConfigData> value_subscription) :
        subscription(std::move(value_subscription)) {
        stop_publisher = stop.acquire_publisher();
        FIBER_ASSERT(stop_publisher.has_value());
    }

    nacos::Subscription<nacos::ConfigData> subscription;
    async::Watch<bool> stop{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher;
    bool stopping = false;
};

struct AccessConfigWatcher::ProjectEntry final : public common::NonCopyable, public common::NonMovable {
    ProjectEntry(AccessConfigWatcher &value_owner, std::string value_project,
                 nacos::Subscription<nacos::ConfigData> value_subscription) :
        owner(&value_owner), project(std::move(value_project)), subscription(std::move(value_subscription)) {
        stop_publisher = stop.acquire_publisher();
        FIBER_ASSERT(stop_publisher.has_value());
    }

    AccessConfigWatcher *owner = nullptr;
    std::string project;
    nacos::Subscription<nacos::ConfigData> subscription;
    async::Watch<bool> stop{false};
    std::optional<async::Watch<bool>::Publisher> stop_publisher;
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
    FIBER_ASSERT(tasks_.empty());
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

    auto subscription = config_service_->subscribe(options_.project_list_data_id, options_.project_route_group);
    if (!subscription) {
        return std::unexpected(std::move(subscription.error()));
    }
    project_list_ = std::make_unique<ProjectListEntry>(std::move(*subscription));
    state_ = AccessConfigWatcherState::Running;
    tasks_.add();
    async::spawn([this]() { return run_project_list(*this); });
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
        request_stop(*project_list_);
        for (auto &[project, entry]: projects_) {
            (void) project;
            request_stop(*entry);
        }
    }
    co_await tasks_.join();
    projects_.clear();
    project_list_.reset();
    state_ = AccessConfigWatcherState::Stopped;
}

async::DetachedTask AccessConfigWatcher::run_project_list(AccessConfigWatcher &owner) noexcept {
    ProjectListEntry &entry = *owner.project_list_;
    auto stop = entry.stop.subscribe();
    auto stop_snapshot = stop.current();
    auto &subscriber = entry.subscription.subscriber();
    auto snapshot = subscriber.current();
    for (;;) {
        if (stop_snapshot.value && *stop_snapshot.value) {
            break;
        }
        if (snapshot.value) {
            if (snapshot.value->kind == nacos::ResultKind::Closed) {
                break;
            }
            if (snapshot.value->data && owner.state_ == AccessConfigWatcherState::Running) {
                owner.apply_project_list(*snapshot.value->data);
            }
        }
        auto result = co_await async::when_any(
                [&subscriber, version = snapshot.version]() { return subscriber.next(version); },
                [&stop, version = stop_snapshot.version]() { return stop.next(version); });
        if (result.is<1>()) {
            std::move(result).get<1>();
            break;
        }
        snapshot = std::move(result).get<0>();
        stop_snapshot = stop.current();
    }
    entry.subscription.close();
    owner.tasks_.done();
}

async::DetachedTask AccessConfigWatcher::run_project(std::shared_ptr<ProjectEntry> entry) noexcept {
    auto stop = entry->stop.subscribe();
    auto stop_snapshot = stop.current();
    auto &subscriber = entry->subscription.subscriber();
    auto snapshot = subscriber.current();
    for (;;) {
        if (stop_snapshot.value && *stop_snapshot.value) {
            break;
        }
        if (snapshot.value) {
            if (snapshot.value->kind == nacos::ResultKind::Closed) {
                break;
            }
            if (snapshot.value->data && !entry->stopping && entry->owner->state_ == AccessConfigWatcherState::Running) {
                entry->owner->apply_project(*entry, *snapshot.value->data);
            }
        }
        auto result = co_await async::when_any(
                [&subscriber, version = snapshot.version]() { return subscriber.next(version); },
                [&stop, version = stop_snapshot.version]() { return stop.next(version); });
        if (result.is<1>()) {
            std::move(result).get<1>();
            break;
        }
        snapshot = std::move(result).get<0>();
        stop_snapshot = stop.current();
    }
    entry->subscription.close();
    entry->owner->tasks_.done();
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

void AccessConfigWatcher::apply_project(ProjectEntry &entry, const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    if (data.state == nacos::ConfigState::NotFound || data.content.empty()) {
        auto ignored = store_->apply(entry.project, std::nullopt);
        FIBER_ASSERT(ignored.has_value());
        return;
    }

    auto parsed = parse_project_config(data.content);
    if (!parsed) {
        report_failure(options_.project_route_data_id_prefix + entry.project, data.md5, std::move(parsed.error()));
        return;
    }
    auto updated = store_->apply(entry.project, *parsed);
    if (!updated) {
        report_failure(options_.project_route_data_id_prefix + entry.project, data.md5, std::move(updated.error()));
        return;
    }

    if (updated->status == ConfigUpdateStatus::Published || updated->status == ConfigUpdateStatus::Unloaded) {
        ++successful_updates_;
        publish_observer(updated->snapshot);
    }
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
    auto subscription = config_service_->subscribe(data_id, options_.project_route_group);
    if (!subscription) {
        ++failed_updates_;
        return;
    }
    auto entry = std::make_shared<ProjectEntry>(*this, std::move(project), std::move(*subscription));
    auto [iterator, inserted] = projects_.emplace(entry->project, entry);
    FIBER_ASSERT(inserted);
    (void) iterator;
    tasks_.add();
    async::spawn([entry = std::move(entry)]() mutable { return run_project(std::move(entry)); });
}

void AccessConfigWatcher::remove_project(std::string_view project) {
    FIBER_ASSERT(loop_->in_loop());
    const auto iterator = projects_.find(project);
    if (iterator == projects_.end()) {
        return;
    }
    std::shared_ptr<ProjectEntry> retiring = std::move(iterator->second);
    projects_.erase(iterator);
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
