#ifndef FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_REGISTRY_H
#define FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_REGISTRY_H

#include <atomic>
#include <cstdint>
#include <memory>

#include "http/LocalHttp1ConnectionPoolSet.h"

#include "../runtime/RuntimeConfig.h"

namespace fiber::event {
class EventLoopGroup;
}

namespace fiber::lite_nginx::upstream {

class UpstreamRegistry {
public:
    struct ConnectionHandle {
        const runtime::UpstreamRuntime *upstream = nullptr;
        const runtime::UpstreamPeerRuntime *peer = nullptr;
        fiber::http::LocalHttp1ConnectionPoolSet::Lease lease{};

        [[nodiscard]] bool valid() const noexcept { return upstream != nullptr && peer != nullptr; }
        [[nodiscard]] bool pooled() const noexcept { return lease.valid(); }
    };

    UpstreamRegistry(fiber::event::EventLoopGroup &group, const runtime::RuntimeConfig &runtime) noexcept;
    ~UpstreamRegistry();

    [[nodiscard]] bool init() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] const runtime::UpstreamPeerRuntime *select_peer(std::uint32_t upstream_index) noexcept;
    [[nodiscard]] ConnectionHandle acquire_connection(std::uint32_t upstream_index) noexcept;

private:
    fiber::event::EventLoopGroup *group_ = nullptr;
    const runtime::RuntimeConfig *runtime_ = nullptr;
    std::unique_ptr<std::atomic<std::uint32_t>[]> cursors_{};
    std::unique_ptr<fiber::http::LocalHttp1ConnectionPoolSet> pools_{};
};

} // namespace fiber::lite_nginx::upstream

#endif // FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_REGISTRY_H
