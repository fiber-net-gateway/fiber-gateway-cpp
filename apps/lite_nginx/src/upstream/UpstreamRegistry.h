#ifndef FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_REGISTRY_H
#define FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_REGISTRY_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include "async/Task.h"
#include "http/Http1ConnectionGroupKey.h"
#include "http/StealableHttp1ConnectionPoolSet.h"
#include "net/SocketAddress.h"
#include "net/TlsOptions.h"

#include "../runtime/RuntimeConfig.h"

namespace fiber::event {
class EventLoopGroup;
}

namespace fiber::lite_nginx::upstream {

// Owns the single global keepalive connection pool shared across all upstreams and ad-hoc
// script targets (http.request({url:...})). Connections are pooled and keyed by peer identity
// (Http1ConnectionGroupKey), so any upstream -- or ad-hoc URL -- that resolves to the same
// ip:port reuses the same idle connections.
//
// Upstream blocks only carry ip:port + weight; pool sizing lives in http.connection_pool.
// Peer selection across an upstream's peers is smooth weighted round-robin (nginx-style),
// guarded per upstream by a mutex (selection is O(peers) and brief).
class UpstreamRegistry {
public:
    // Wraps a stealable-pool lease. Move-only; empty when no pool is configured
    // (keepalive_size == 0) so the caller falls back to a transient connection.
    class PooledLease {
    public:
        PooledLease() noexcept = default;
        explicit PooledLease(fiber::http::StealableHttp1ConnectionPoolSet::Lease &&lease) noexcept;
        PooledLease(PooledLease &&other) noexcept;
        PooledLease &operator=(PooledLease &&other) noexcept;
        ~PooledLease() = default;

        PooledLease(const PooledLease &) = delete;
        PooledLease &operator=(const PooledLease &) = delete;

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] fiber::http::Http1ClientConnection *get() noexcept;
        [[nodiscard]] fiber::common::IoResult<fiber::http::Http1ClientConnection *>
        emplace_connection(fiber::http::Http1ClientConnectionOptions options) noexcept;
        void reset() noexcept;

    private:
        fiber::http::StealableHttp1ConnectionPoolSet::Lease lease_{};
    };

    struct ConnectionHandle {
        fiber::net::SocketAddress peer_addr{};
        fiber::net::TlsOptions tls{};
        PooledLease lease{};
        bool valid_flag = false;

        [[nodiscard]] bool valid() const noexcept { return valid_flag; }
        [[nodiscard]] bool pooled() const noexcept { return lease.valid(); }
    };

    UpstreamRegistry(fiber::event::EventLoopGroup &group, const runtime::RuntimeConfig &runtime) noexcept;
    ~UpstreamRegistry();

    [[nodiscard]] bool init() noexcept;
    void shutdown() noexcept;

    // Resolve an upstream by name (leading '@' is accepted and stripped). nullptr if unknown.
    [[nodiscard]] const runtime::UpstreamRuntime *find_upstream(std::string_view name) const noexcept;

    // Smooth weighted round-robin peer selection for an upstream. nullptr if out of range / no peers.
    [[nodiscard]] const runtime::UpstreamPeerRuntime *select_peer(std::uint32_t upstream_index) noexcept;

    // Acquire a connection to a configured upstream's selected (weighted) peer.
    [[nodiscard]] fiber::async::Task<ConnectionHandle> acquire_connection(std::uint32_t upstream_index) noexcept;
    // Acquire by upstream name (leading '@' accepted). Invalid handle if unknown.
    [[nodiscard]] fiber::async::Task<ConnectionHandle> acquire_by_name(std::string_view name) noexcept;
    // Acquire to an ad-hoc peer key (e.g. a DNS-resolved URL). When the pool has no idle
    // connection for the key, the caller connects a fresh connection to peer_addr.
    [[nodiscard]] fiber::async::Task<ConnectionHandle> acquire_by_key(const fiber::http::Http1ConnectionGroupKey &key,
                                                                      fiber::net::SocketAddress peer_addr,
                                                                      fiber::net::TlsOptions tls) noexcept;

    [[nodiscard]] fiber::http::StealableHttp1ConnectionPoolSet *pool() noexcept { return pool_.get(); }

private:
    struct UpstreamState {
        std::mutex mu;
        std::vector<std::int64_t> current_weights;
    };

    [[nodiscard]] fiber::async::Task<void> shutdown_async() noexcept;

    fiber::event::EventLoopGroup *group_ = nullptr;
    const runtime::RuntimeConfig *runtime_ = nullptr;
    std::unique_ptr<fiber::http::StealableHttp1ConnectionPoolSet> pool_;
    std::unique_ptr<UpstreamState[]> states_{};
};

} // namespace fiber::lite_nginx::upstream

#endif // FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_REGISTRY_H
