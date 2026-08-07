#include "GrayConfigWatcher.h"

#include "../config/AccessConfigCodec.h"

#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::access_server {

GrayConfigWatcher::GrayConfigWatcher(event::EventLoop &loop, nacos::ConfigService &config_service,
                                     GrayMatchStore &store, GrayConfigWatcherOptions options) :
    loop_(&loop), config_service_(&config_service), store_(&store), options_(std::move(options)) {}

GrayConfigWatcher::~GrayConfigWatcher() {
    FIBER_ASSERT(state_ == GrayConfigWatcherState::Created || state_ == GrayConfigWatcherState::Stopped);
    FIBER_ASSERT(!subscription_);
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
    state_ = GrayConfigWatcherState::Running;
    auto subscription = config_service_->subscribe(options_.data_id, options_.group, &on_notify, this);
    if (!subscription) {
        state_ = GrayConfigWatcherState::Created;
        return std::unexpected(std::move(subscription.error()));
    }
    subscription_.emplace(std::move(*subscription));
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
    request_stop();
    subscription_.reset();
    state_ = GrayConfigWatcherState::Stopped;
}

void GrayConfigWatcher::on_notify(void *context, const nacos::SubscriptionResult<nacos::ConfigData> &result) noexcept {
    auto &owner = *static_cast<GrayConfigWatcher *>(context);
    if (result.kind == nacos::ResultKind::Closed) {
        owner.request_stop();
        return;
    }
    if (result.data && owner.state_ == GrayConfigWatcherState::Running) {
        owner.apply(*result.data);
    }
}

void GrayConfigWatcher::apply(const nacos::ConfigData &data) {
    FIBER_ASSERT(loop_->in_loop());
    const std::string_view content =
            data.state == nacos::ConfigState::NotFound ? std::string_view{} : std::string_view(data.content);
    auto parsed = parse_gray_match_config(content);
    if (!parsed) {
        ++failed_updates_;
        last_failure_ = GrayConfigWatcherFailure{
                .md5 = std::string(data.md5),
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

void GrayConfigWatcher::request_stop() noexcept {
    if (subscription_) {
        subscription_->close();
    }
}

} // namespace fiber::access_server
