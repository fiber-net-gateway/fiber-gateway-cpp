#include "UpstreamRegistry.h"

#include <limits>
#include <utility>

#include <fiber/event/EventLoopGroup.h>

namespace fiber::lite_nginx::upstream {
namespace {

std::string_view strip_at(std::string_view name) noexcept {
    if (!name.empty() && name.front() == '@') {
        name.remove_prefix(1);
    }
    return name;
}

} // namespace

UpstreamRegistry::UpstreamRegistry(fiber::event::EventLoopGroup &group, const runtime::RuntimeConfig &runtime) noexcept
    :
    group_(&group), runtime_(&runtime),
    states_(runtime.upstreams.empty() ? nullptr : std::make_unique<UpstreamState[]>(runtime.upstreams.size())) {
    for (std::size_t i = 0; i < runtime.upstreams.size(); ++i) {
        states_[i].current_weights.assign(runtime.upstreams[i].peers.size(), 0);
    }
}

UpstreamRegistry::~UpstreamRegistry() = default;

bool UpstreamRegistry::init() noexcept { return true; }

void UpstreamRegistry::shutdown() noexcept { states_.reset(); }

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

const runtime::UpstreamPeerRuntime *UpstreamRegistry::select_by_name(std::string_view name) noexcept {
    const auto *upstream = find_upstream(name);
    if (!upstream) {
        return nullptr;
    }
    const std::uint32_t index = static_cast<std::uint32_t>(upstream - runtime_->upstreams.data());
    return select_peer(index);
}

} // namespace fiber::lite_nginx::upstream
