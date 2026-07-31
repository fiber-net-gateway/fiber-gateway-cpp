#include "GrayConfigWatcher.h"

#include "../config/AccessConfigCodec.h"

#include <utility>

#include <async/WhenAny.h>
#include <common/Assert.h>

namespace fiber::access_server {

GrayConfigWatcher::GrayConfigWatcher(event::EventLoop &loop, nacos::ConfigService &config_service,
                                     GrayMatchStore &store, GrayConfigWatcherOptions options) :
    loop_(&loop), config_service_(&config_service), store_(&store), options_(std::move(options)) {
    stop_publisher_ = stop_.acquire_publisher();
    FIBER_ASSERT(stop_publisher_.has_value());
}

GrayConfigWatcher::~GrayConfigWatcher() {
    FIBER_ASSERT(state_ == GrayConfigWatcherState::Created || state_ == GrayConfigWatcherState::Stopped);
    FIBER_ASSERT(!subscription_);
    FIBER_ASSERT(tasks_.empty());
}

std::expected<void, nacos::ConfigServiceError> GrayConfigWatcher::start() {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != GrayConfigWatcherState::Created) {
        return std::unexpected(nacos::ConfigServiceError{
                .code = nacos::ConfigServiceErrorCode::InvalidArgument,
                .io_error = common::IoErr::Already,
                .message = "gray config watcher is already started",
        });
    }
    auto subscription = config_service_->subscribe(options_.data_id, options_.group);
    if (!subscription) {
        return std::unexpected(std::move(subscription.error()));
    }
    subscription_.emplace(std::move(*subscription));
    state_ = GrayConfigWatcherState::Running;
    tasks_.add();
    async::spawn([this]() { return run(*this); });
    return {};
}

async::Task<void> GrayConfigWatcher::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ == GrayConfigWatcherState::Stopped) {
        co_return;
    }
    if (state_ == GrayConfigWatcherState::Created) {
        state_ = GrayConfigWatcherState::Stopped;
        co_return;
    }
    if (state_ == GrayConfigWatcherState::Running) {
        state_ = GrayConfigWatcherState::Stopping;
        request_stop();
    }
    co_await tasks_.join();
    subscription_.reset();
    state_ = GrayConfigWatcherState::Stopped;
}

async::DetachedTask GrayConfigWatcher::run(GrayConfigWatcher &owner) noexcept {
    auto stop = owner.stop_.subscribe();
    auto stop_snapshot = stop.current();
    auto &subscriber = owner.subscription_->subscriber();
    auto snapshot = subscriber.current();
    for (;;) {
        if (stop_snapshot.value && *stop_snapshot.value) {
            break;
        }
        if (snapshot.value) {
            if (snapshot.value->kind == nacos::ResultKind::Closed) {
                break;
            }
            if (snapshot.value->data && owner.state_ == GrayConfigWatcherState::Running) {
                owner.apply(*snapshot.value->data);
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
    owner.subscription_->close();
    owner.tasks_.done();
}

void GrayConfigWatcher::apply(const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    const std::string_view content =
            data.state == nacos::ConfigState::NotFound ? std::string_view{} : std::string_view(data.content);
    auto parsed = parse_gray_match_config(content);
    if (!parsed) {
        ++failed_updates_;
        last_failure_ = GrayConfigWatcherFailure{
                .md5 = data.md5,
                .error = std::move(parsed.error()),
        };
        return;
    }
    auto updated = store_->apply(*parsed);
    FIBER_ASSERT(updated.has_value());
    if (*updated == GrayMatchUpdateStatus::Published) {
        ++successful_updates_;
    }
}

void GrayConfigWatcher::request_stop() noexcept { stop_publisher_->publish(true); }

} // namespace fiber::access_server
