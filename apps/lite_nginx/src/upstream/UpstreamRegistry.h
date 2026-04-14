#ifndef FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_REGISTRY_H
#define FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_REGISTRY_H

#include <atomic>
#include <cstdint>
#include <memory>

#include "async/Task.h"
#include "http/LocalHttp1ConnectionPoolSet.h"
#include "http/StealableHttp1ConnectionPoolSet.h"

#include "../runtime/RuntimeConfig.h"

namespace fiber::event {
class EventLoopGroup;
}

namespace fiber::lite_nginx::upstream {

class UpstreamRegistry {
public:
    class PooledLease {
    public:
        PooledLease() noexcept = default;
        explicit PooledLease(fiber::http::LocalHttp1ConnectionPoolSet::Lease &&lease) noexcept;
        explicit PooledLease(fiber::http::StealableHttp1ConnectionPoolSet::Lease &&lease) noexcept;
        PooledLease(PooledLease &&other) noexcept;
        PooledLease &operator=(PooledLease &&other) noexcept;
        ~PooledLease() = default;

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] fiber::http::Http1ClientConnection *get() noexcept;
        [[nodiscard]] fiber::common::IoResult<fiber::http::Http1ClientConnection *>
        emplace_connection(fiber::http::Http1ClientConnectionOptions options) noexcept;
        void reset() noexcept;

    private:
        enum class Kind : std::uint8_t { Empty, Local, Stealable };

        Kind kind_ = Kind::Empty;
        fiber::http::LocalHttp1ConnectionPoolSet::Lease local_{};
        fiber::http::StealableHttp1ConnectionPoolSet::Lease stealable_{};
    };

    struct ConnectionHandle {
        const runtime::UpstreamRuntime *upstream = nullptr;
        const runtime::UpstreamPeerRuntime *peer = nullptr;
        PooledLease lease{};

        [[nodiscard]] bool valid() const noexcept { return upstream != nullptr && peer != nullptr; }
        [[nodiscard]] bool pooled() const noexcept { return lease.valid(); }
    };

    UpstreamRegistry(fiber::event::EventLoopGroup &group, const runtime::RuntimeConfig &runtime) noexcept;
    ~UpstreamRegistry();

    [[nodiscard]] bool init() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] const runtime::UpstreamPeerRuntime *select_peer(std::uint32_t upstream_index) noexcept;
    [[nodiscard]] fiber::async::Task<ConnectionHandle> acquire_connection(std::uint32_t upstream_index) noexcept;

private:
    struct UpstreamState {
        std::atomic<std::uint32_t> cursor{0};
        std::unique_ptr<fiber::http::LocalHttp1ConnectionPoolSet> local_pool{};
        std::unique_ptr<fiber::http::StealableHttp1ConnectionPoolSet> stealable_pool{};
    };

    [[nodiscard]] fiber::async::Task<void> shutdown_async() noexcept;

    fiber::event::EventLoopGroup *group_ = nullptr;
    const runtime::RuntimeConfig *runtime_ = nullptr;
    std::unique_ptr<UpstreamState[]> states_{};
};

} // namespace fiber::lite_nginx::upstream

#endif // FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_REGISTRY_H
