#ifndef FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_REGISTRY_H
#define FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_REGISTRY_H

#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include "../runtime/RuntimeConfig.h"

namespace fiber::event {
class EventLoopGroup;
}

namespace fiber::lite_nginx::upstream {

// Pure upstream *selector*. Each upstream block carries ip:port/host + weight (+ scheme via the
// peer's connection_key); this class performs smooth weighted round-robin peer selection only.
// Connection pooling lives in ConnectionPool (global, keyed by the selected peer's
// Http1ConnectionGroupKey); DNS resolution for name peers happens at connect time in the unified
// acquire_and_connect path. This split keeps upstream = "pick a peer", pool = "reuse / create".
class UpstreamRegistry {
public:
    UpstreamRegistry(fiber::event::EventLoopGroup &group, const runtime::RuntimeConfig &runtime) noexcept;
    ~UpstreamRegistry();

    [[nodiscard]] bool init() noexcept;
    void shutdown() noexcept;

    // Resolve an upstream by name (leading '@' is accepted and stripped). nullptr if unknown.
    [[nodiscard]] const runtime::UpstreamRuntime *find_upstream(std::string_view name) const noexcept;

    // Smooth weighted round-robin peer selection for an upstream. nullptr if out of range / no peers.
    [[nodiscard]] const runtime::UpstreamPeerRuntime *select_peer(std::uint32_t upstream_index) noexcept;

    // Select a peer for an upstream identified by name (leading '@' accepted). nullptr if unknown.
    [[nodiscard]] const runtime::UpstreamPeerRuntime *select_by_name(std::string_view name) noexcept;

private:
    struct UpstreamState {
        std::mutex mu;
        std::vector<std::int64_t> current_weights;
    };

    fiber::event::EventLoopGroup *group_ = nullptr;
    const runtime::RuntimeConfig *runtime_ = nullptr;
    std::unique_ptr<UpstreamState[]> states_{};
};

} // namespace fiber::lite_nginx::upstream

#endif // FIBER_LITE_NGINX_UPSTREAM_UPSTREAM_REGISTRY_H
