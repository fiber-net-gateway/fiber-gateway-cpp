#include "UpstreamRegistry.h"

#include <chrono>
#include <future>
#include <utility>

#include "async/Spawn.h"
#include "event/EventLoopGroup.h"

namespace fiber::lite_nginx::upstream {
namespace {

fiber::http::LocalHttp1ConnectionPoolSet::Options make_pool_options(const runtime::UpstreamRuntime &upstream) noexcept {
    fiber::http::LocalHttp1ConnectionPoolSet::Options options{};
    options.max_idle_per_group = upstream.keepalive;
    options.max_idle_total = upstream.keepalive;
    options.initial_group_capacity = upstream.peers.size();
    options.idle_timeout = std::chrono::milliseconds(30000);
    return options;
}

bool should_create_pool(const runtime::UpstreamRuntime &upstream) noexcept {
    return !upstream.name.empty() && upstream.keepalive > 0;
}

} // namespace

UpstreamRegistry::PooledLease::PooledLease(fiber::http::LocalHttp1ConnectionPoolSet::Lease &&lease) noexcept :
    kind_(Kind::Local), local_(std::move(lease)) {}

UpstreamRegistry::PooledLease::PooledLease(fiber::http::StealableHttp1ConnectionPoolSet::Lease &&lease) noexcept :
    kind_(Kind::Stealable), stealable_(std::move(lease)) {}

UpstreamRegistry::PooledLease::PooledLease(PooledLease &&other) noexcept :
    kind_(other.kind_), local_(std::move(other.local_)), stealable_(std::move(other.stealable_)) {
    other.kind_ = Kind::Empty;
}

UpstreamRegistry::PooledLease &UpstreamRegistry::PooledLease::operator=(PooledLease &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    kind_ = other.kind_;
    local_ = std::move(other.local_);
    stealable_ = std::move(other.stealable_);
    other.kind_ = Kind::Empty;
    return *this;
}

bool UpstreamRegistry::PooledLease::valid() const noexcept {
    switch (kind_) {
        case Kind::Local:
            return local_.valid();
        case Kind::Stealable:
            return stealable_.valid();
        case Kind::Empty:
            return false;
    }
    return false;
}

fiber::http::Http1ClientConnection *UpstreamRegistry::PooledLease::get() noexcept {
    switch (kind_) {
        case Kind::Local:
            return local_.get();
        case Kind::Stealable:
            return stealable_.get();
        case Kind::Empty:
            return nullptr;
    }
    return nullptr;
}

fiber::common::IoResult<fiber::http::Http1ClientConnection *>
UpstreamRegistry::PooledLease::emplace_connection(fiber::http::Http1ClientConnectionOptions options) noexcept {
    switch (kind_) {
        case Kind::Local:
            return local_.emplace_connection(std::move(options));
        case Kind::Stealable:
            return stealable_.emplace_connection(std::move(options));
        case Kind::Empty:
            return std::unexpected(fiber::common::IoErr::Invalid);
    }
    return std::unexpected(fiber::common::IoErr::Invalid);
}

void UpstreamRegistry::PooledLease::reset() noexcept {
    switch (kind_) {
        case Kind::Local:
            local_.reset();
            break;
        case Kind::Stealable:
            stealable_.reset();
            break;
        case Kind::Empty:
            break;
    }
    kind_ = Kind::Empty;
}

UpstreamRegistry::UpstreamRegistry(fiber::event::EventLoopGroup &group, const runtime::RuntimeConfig &runtime) noexcept
    :
    group_(&group), runtime_(&runtime),
    states_(runtime.upstreams.empty() ? nullptr : std::make_unique<UpstreamState[]>(runtime.upstreams.size())) {
    for (std::size_t i = 0; i < runtime.upstreams.size(); ++i) {
        states_[i].cursor.store(0, std::memory_order_relaxed);
        const auto &upstream = runtime.upstreams[i];
        if (!should_create_pool(upstream)) {
            continue;
        }
        auto options = make_pool_options(upstream);
        if (upstream.keepalive_mode == runtime::KeepaliveMode::Stealable) {
            states_[i].stealable_pool = std::make_unique<fiber::http::StealableHttp1ConnectionPoolSet>(group, options);
        } else {
            states_[i].local_pool = std::make_unique<fiber::http::LocalHttp1ConnectionPoolSet>(group, options);
        }
    }
}

UpstreamRegistry::~UpstreamRegistry() = default;

bool UpstreamRegistry::init() noexcept {
    if (!runtime_ || !states_) {
        return true;
    }
    for (std::size_t i = 0; i < runtime_->upstreams.size(); ++i) {
        auto &state = states_[i];
        if (state.local_pool && !state.local_pool->init()) {
            return false;
        }
        if (state.stealable_pool && !state.stealable_pool->init()) {
            return false;
        }
    }
    return true;
}

fiber::async::Task<void> UpstreamRegistry::shutdown_async() noexcept {
    if (!runtime_ || !states_) {
        co_return;
    }
    for (std::size_t i = 0; i < runtime_->upstreams.size(); ++i) {
        auto &state = states_[i];
        if (state.local_pool) {
            co_await state.local_pool->shutdown_async();
        }
        if (state.stealable_pool) {
            co_await state.stealable_pool->shutdown_async();
        }
    }
}

void UpstreamRegistry::shutdown() noexcept {
    if (!states_) {
        return;
    }
    if (!group_ || group_->size() == 0 || !group_->running()) {
        states_.reset();
        return;
    }

    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    fiber::async::spawn(group_->at(0), [this, done]() -> fiber::async::DetachedTask {
        co_await shutdown_async();
        done->set_value();
    });
    future.wait();
    states_.reset();
}

const runtime::UpstreamPeerRuntime *UpstreamRegistry::select_peer(std::uint32_t upstream_index) noexcept {
    if (!runtime_ || upstream_index >= runtime_->upstreams.size()) {
        return nullptr;
    }
    const runtime::UpstreamRuntime &upstream = runtime_->upstreams[upstream_index];
    if (upstream.peers.empty()) {
        return nullptr;
    }
    if (upstream.peers.size() == 1) {
        return &upstream.peers.front();
    }

    const std::uint32_t cursor = states_[upstream_index].cursor.fetch_add(1, std::memory_order_relaxed);
    return &upstream.peers[cursor % upstream.peers.size()];
}

fiber::async::Task<UpstreamRegistry::ConnectionHandle>
UpstreamRegistry::acquire_connection(std::uint32_t upstream_index) noexcept {
    ConnectionHandle handle;
    if (!runtime_ || upstream_index >= runtime_->upstreams.size()) {
        co_return handle;
    }

    handle.upstream = &runtime_->upstreams[upstream_index];
    handle.peer = select_peer(upstream_index);
    if (!handle.peer) {
        handle.upstream = nullptr;
        co_return handle;
    }

    if (!states_) {
        co_return handle;
    }

    auto &state = states_[upstream_index];
    if (state.local_pool && handle.peer->connection_key.has_value()) {
        handle.lease = PooledLease(state.local_pool->acquire(*handle.peer->connection_key));
    } else if (state.stealable_pool && handle.peer->connection_key.has_value()) {
        handle.lease = PooledLease(co_await state.stealable_pool->acquire(*handle.peer->connection_key));
    }
    co_return handle;
}

} // namespace fiber::lite_nginx::upstream
