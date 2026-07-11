#include "UpstreamRegistry.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <utility>

#include "async/Spawn.h"
#include "event/EventLoopGroup.h"

namespace fiber::lite_nginx::upstream {
namespace {

// One global pool serves all peers; cap idle per peer group and overall per core.
fiber::http::StealableHttp1ConnectionPoolSet::Options
make_pool_options(const runtime::ConnectionPoolRuntime &cp) noexcept {
    fiber::http::StealableHttp1ConnectionPoolSet::Options options{};
    options.max_idle_per_group = cp.keepalive_size;
    options.max_idle_total = cp.keepalive_size * 64;
    options.initial_group_capacity = 16;
    options.idle_timeout = cp.keepalive_timeout;
    return options;
}

std::string_view strip_at(std::string_view name) noexcept {
    if (!name.empty() && name.front() == '@') {
        name.remove_prefix(1);
    }
    return name;
}

} // namespace

UpstreamRegistry::PooledLease::PooledLease(fiber::http::StealableHttp1ConnectionPoolSet::Lease &&lease) noexcept :
    lease_(std::move(lease)) {}

UpstreamRegistry::PooledLease::PooledLease(PooledLease &&other) noexcept : lease_(std::move(other.lease_)) {}

UpstreamRegistry::PooledLease &UpstreamRegistry::PooledLease::operator=(PooledLease &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    lease_ = std::move(other.lease_);
    return *this;
}

bool UpstreamRegistry::PooledLease::valid() const noexcept { return lease_.valid(); }

fiber::http::Http1ClientConnection *UpstreamRegistry::PooledLease::get() noexcept { return lease_.get(); }

fiber::common::IoResult<fiber::http::Http1ClientConnection *>
UpstreamRegistry::PooledLease::emplace_connection(fiber::http::Http1ClientConnectionOptions options) noexcept {
    return lease_.emplace_connection(std::move(options));
}

void UpstreamRegistry::PooledLease::reset() noexcept { lease_.reset(); }

UpstreamRegistry::UpstreamRegistry(fiber::event::EventLoopGroup &group, const runtime::RuntimeConfig &runtime) noexcept
    :
    group_(&group), runtime_(&runtime),
    states_(runtime.upstreams.empty() ? nullptr : std::make_unique<UpstreamState[]>(runtime.upstreams.size())) {
    for (std::size_t i = 0; i < runtime.upstreams.size(); ++i) {
        states_[i].current_weights.assign(runtime.upstreams[i].peers.size(), 0);
    }
    if (runtime.connection_pool.keepalive_size > 0) {
        pool_ = std::make_unique<fiber::http::StealableHttp1ConnectionPoolSet>(
                group, make_pool_options(runtime.connection_pool));
    }
}

UpstreamRegistry::~UpstreamRegistry() = default;

bool UpstreamRegistry::init() noexcept {
    if (pool_ && !pool_->init()) {
        return false;
    }
    return true;
}

fiber::async::Task<void> UpstreamRegistry::shutdown_async() noexcept {
    if (pool_) {
        co_await pool_->shutdown_async();
    }
    co_return;
}

void UpstreamRegistry::shutdown() noexcept {
    if (!pool_ || !group_ || group_->size() == 0 || !group_->running()) {
        pool_.reset();
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
    pool_.reset();
    states_.reset();
}

const runtime::UpstreamRuntime *UpstreamRegistry::find_upstream(std::string_view name) const noexcept {
    if (!runtime_) {
        return nullptr;
    }
    const std::string_view key = strip_at(name);
    for (const auto &upstream: runtime_->upstreams) {
        if (upstream.name == key) {
            return &upstream;
        }
    }
    return nullptr;
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

    // Smooth weighted round-robin (nginx algorithm), guarded per upstream.
    auto &state = states_[upstream_index];
    std::lock_guard<std::mutex> lock(state.mu);
    auto &cw = state.current_weights;
    if (cw.size() != upstream.peers.size()) {
        cw.assign(upstream.peers.size(), 0);
    }
    std::int64_t total = 0;
    std::size_t best = 0;
    std::int64_t best_cw = std::numeric_limits<std::int64_t>::min();
    for (std::size_t i = 0; i < upstream.peers.size(); ++i) {
        const std::int64_t w = static_cast<std::int64_t>(upstream.peers[i].weight);
        cw[i] += w;
        total += w;
        if (cw[i] > best_cw) {
            best_cw = cw[i];
            best = i;
        }
    }
    cw[best] -= total;
    return &upstream.peers[best];
}

fiber::async::Task<UpstreamRegistry::ConnectionHandle>
UpstreamRegistry::acquire_connection(std::uint32_t upstream_index) noexcept {
    ConnectionHandle handle;
    if (!runtime_ || upstream_index >= runtime_->upstreams.size()) {
        co_return handle;
    }
    const auto *peer = select_peer(upstream_index);
    if (!peer) {
        co_return handle;
    }
    handle.peer_addr = peer->address;
    handle.valid_flag = true;
    if (pool_ && peer->connection_key.has_value()) {
        auto lease = co_await pool_->acquire(*peer->connection_key);
        handle.lease = PooledLease(std::move(lease));
    }
    co_return handle;
}

fiber::async::Task<UpstreamRegistry::ConnectionHandle>
UpstreamRegistry::acquire_by_name(std::string_view name) noexcept {
    const auto *upstream = find_upstream(name);
    if (!upstream) {
        co_return ConnectionHandle{};
    }
    // Locate the upstream index to drive weighted selection.
    const std::uint32_t index = static_cast<std::uint32_t>(upstream - runtime_->upstreams.data());
    co_return co_await acquire_connection(index);
}

fiber::async::Task<UpstreamRegistry::ConnectionHandle>
UpstreamRegistry::acquire_by_key(const fiber::http::Http1ConnectionGroupKey &key, fiber::net::SocketAddress peer_addr,
                                 fiber::net::TlsOptions tls) noexcept {
    ConnectionHandle handle;
    handle.peer_addr = peer_addr;
    handle.tls = std::move(tls);
    handle.valid_flag = true;
    if (pool_) {
        auto lease = co_await pool_->acquire(key);
        handle.lease = PooledLease(std::move(lease));
    }
    co_return handle;
}

} // namespace fiber::lite_nginx::upstream
