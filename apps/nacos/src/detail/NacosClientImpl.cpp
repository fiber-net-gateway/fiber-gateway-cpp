#include "NacosClientImpl.h"

#include <utility>

#include <common/Assert.h>

#include "../auth/NacosAuthenticator.h"

namespace fiber::nacos::detail {

NacosClientImpl::NacosClientImpl(event::EventLoop &loop, NacosClientConfig config, NacosClientOptions options) :
    loop_(&loop), config_(std::move(config)), options_(options) {
    shutdown_publisher_ = shutdown_watch_.acquire_publisher();
    auth_publisher_ = auth_watch_.acquire_publisher();
    FIBER_ASSERT(shutdown_publisher_.has_value());
    FIBER_ASSERT(auth_publisher_.has_value());
    authenticator_ = std::make_unique<NacosAuthenticator>(*this);
}

NacosClientImpl::~NacosClientImpl() {
    FIBER_ASSERT(state_ == NacosClientState::Created || state_ == NacosClientState::Stopped);
    FIBER_ASSERT(task_group_.empty());
}

common::IoResult<void> NacosClientImpl::start() noexcept {
    if (!loop_->in_loop()) {
        return std::unexpected(common::IoErr::NotSupported);
    }
    if (state_ != NacosClientState::Created) {
        return std::unexpected(common::IoErr::Already);
    }
    state_ = NacosClientState::Running;
    authenticator_->start();
    return {};
}

async::Task<void> NacosClientImpl::shutdown() noexcept {
    FIBER_ASSERT(loop_->in_loop());

    if (state_ == NacosClientState::Stopped) {
        co_return;
    }
    if (state_ == NacosClientState::Created || state_ == NacosClientState::Running) {
        state_ = NacosClientState::Stopping;
        shutdown_publisher_->publish(true);
        authenticator_->stop();
    }

    co_await task_group_.join();
    if (state_ != NacosClientState::Stopped) {
        authenticator_->publish_stopped();
        state_ = NacosClientState::Stopped;
    }
}

async::Watch<NacosAuthSnapshot>::Subscriber NacosClientImpl::subscribe_auth() { return auth_watch_.subscribe(); }

async::Watch<bool>::Subscriber NacosClientImpl::subscribe_shutdown() { return shutdown_watch_.subscribe(); }

bool NacosClientImpl::try_begin_task() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (state_ != NacosClientState::Running) {
        return false;
    }
    task_group_.add();
    return true;
}

void NacosClientImpl::end_task() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    task_group_.done();
}

void NacosClientImpl::publish_auth(NacosAuthSnapshot snapshot) {
    FIBER_ASSERT(loop_->in_loop());
    auth_publisher_->publish(std::move(snapshot));
}

} // namespace fiber::nacos::detail
