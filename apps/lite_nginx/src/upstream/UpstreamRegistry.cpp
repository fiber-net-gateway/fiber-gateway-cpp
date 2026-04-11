#include "UpstreamRegistry.h"

#include <algorithm>
#include <chrono>
#include <future>

#include "async/Spawn.h"
#include "event/EventLoopGroup.h"

namespace fiber::lite_nginx::upstream {
namespace {

fiber::http::LocalHttp1ConnectionPoolSet::Options make_pool_options(const runtime::RuntimeConfig &runtime) noexcept {
    fiber::http::LocalHttp1ConnectionPoolSet::Options options{};
    std::size_t max_idle_per_group = 0;
    std::size_t max_idle_total = 0;
    std::size_t initial_group_capacity = 0;

    for (const auto &upstream : runtime.upstreams) {
        if (upstream.keepalive == 0) {
            continue;
        }
        max_idle_per_group = std::max(max_idle_per_group, upstream.keepalive);
        max_idle_total += upstream.keepalive * upstream.peers.size();
        initial_group_capacity += upstream.peers.size();
    }

    options.max_idle_per_group = max_idle_per_group;
    options.max_idle_total = max_idle_total;
    options.initial_group_capacity = initial_group_capacity;
    options.idle_timeout = std::chrono::milliseconds(30000);
    return options;
}

} // namespace

UpstreamRegistry::UpstreamRegistry(fiber::event::EventLoopGroup &group, const runtime::RuntimeConfig &runtime) noexcept
    : group_(&group),
      runtime_(&runtime),
      cursors_(runtime.upstreams.empty() ? nullptr : std::make_unique<std::atomic<std::uint32_t>[]>(runtime.upstreams.size())) {
    for (std::size_t i = 0; i < runtime.upstreams.size(); ++i) {
        cursors_[i].store(0, std::memory_order_relaxed);
    }
    pools_ = std::make_unique<fiber::http::LocalHttp1ConnectionPoolSet>(group, make_pool_options(runtime));
}

UpstreamRegistry::~UpstreamRegistry() = default;

bool UpstreamRegistry::init() noexcept {
    return pools_ ? pools_->init() : true;
}

void UpstreamRegistry::shutdown() noexcept {
    if (!pools_) {
        return;
    }
    if (!group_ || group_->size() == 0 || !group_->running()) {
        pools_.reset();
        return;
    }

    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    fiber::async::spawn(group_->at(0), [this, done]() -> fiber::async::DetachedTask {
        co_await pools_->shutdown_async();
        done->set_value();
    });
    future.wait();
    pools_.reset();
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

    const std::uint32_t cursor = cursors_[upstream_index].fetch_add(1, std::memory_order_relaxed);
    return &upstream.peers[cursor % upstream.peers.size()];
}

UpstreamRegistry::ConnectionHandle UpstreamRegistry::acquire_connection(std::uint32_t upstream_index) noexcept {
    ConnectionHandle handle;
    if (!runtime_ || upstream_index >= runtime_->upstreams.size()) {
        return handle;
    }

    handle.upstream = &runtime_->upstreams[upstream_index];
    handle.peer = select_peer(upstream_index);
    if (!handle.peer) {
        handle.upstream = nullptr;
        return handle;
    }

    if (handle.upstream->keepalive > 0 && pools_ && handle.peer->connection_key.has_value()) {
        handle.lease = pools_->acquire(*handle.peer->connection_key);
    }
    return handle;
}

} // namespace fiber::lite_nginx::upstream
