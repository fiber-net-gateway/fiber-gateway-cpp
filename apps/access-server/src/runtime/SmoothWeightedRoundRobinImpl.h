#ifndef FIBER_ACCESS_SERVER_SMOOTH_WEIGHTED_ROUND_ROBIN_IMPL_H
#define FIBER_ACCESS_SERVER_SMOOTH_WEIGHTED_ROUND_ROBIN_IMPL_H

#include "SmoothWeightedRoundRobin.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <utility>

#include <common/Assert.h>
#include <event/EventLoop.h>

namespace fiber::access_server {

template<std::equality_comparable Instance>
struct SmoothWeightedRoundRobin<Instance>::Core {
    explicit Core(Options value_options) noexcept : options(std::move(value_options)) {}

    Options options;
    std::mutex mutex;
    std::shared_ptr<detail::SwrrGeneration<Instance>> current;
};

namespace detail {

struct SwrrPeerRuntime {
    std::int64_t base_weight = 1;
    std::int64_t effective_weight = 1;
    std::int64_t current_weight = 0;
    std::size_t fails = 0;
    std::chrono::steady_clock::time_point accessed{};
    std::chrono::steady_clock::time_point checked{};
    std::uint64_t selection_token = 0;
    bool retired = false;
};

template<std::equality_comparable Instance>
struct SwrrPeer {
    typename SmoothWeightedRoundRobin<Instance>::WeightedInstance value;
    std::int64_t normalized_weight = 1;
    std::shared_ptr<SwrrPeerRuntime> runtime;
};

template<typename Instance>
struct SwrrGeneration {
    std::shared_ptr<typename SmoothWeightedRoundRobin<Instance>::Core> core;
    std::uint64_t generation = 0;
    std::vector<SwrrPeer<Instance>> peers;
};

template<std::equality_comparable Instance>
bool swrr_same_definition(const SwrrPeer<Instance> &left, const SwrrPeer<Instance> &right) noexcept {
    return left.value.selection_token == right.value.selection_token && left.value.instance == right.value.instance &&
           left.value.weight == right.value.weight && left.normalized_weight == right.normalized_weight;
}

template<std::equality_comparable Instance>
bool swrr_same_generation(const SwrrGeneration<Instance> &current, const SwrrGeneration<Instance> &next) noexcept {
    if (current.peers.size() != next.peers.size()) {
        return false;
    }
    for (std::size_t i = 0; i < current.peers.size(); ++i) {
        if (!swrr_same_definition(current.peers[i], next.peers[i])) {
            return false;
        }
    }
    return true;
}

template<std::equality_comparable Instance>
std::int64_t swrr_quantize_weight(double weight,
                                  const typename SmoothWeightedRoundRobin<Instance>::Options &options) noexcept {
    const long double scaled = static_cast<long double>(weight) * options.weight_precision;
    if (scaled >= static_cast<long double>(options.max_normalized_weight)) {
        return options.max_normalized_weight;
    }
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(scaled)));
}

template<std::equality_comparable Instance>
void swrr_normalize_weights(std::vector<SwrrPeer<Instance>> &peers,
                            const typename SmoothWeightedRoundRobin<Instance>::Options &options) noexcept {
    std::int64_t divisor = 0;
    for (SwrrPeer<Instance> &peer: peers) {
        peer.normalized_weight = swrr_quantize_weight<Instance>(peer.value.weight, options);
        divisor = std::gcd(divisor, peer.normalized_weight);
    }
    if (divisor > 1) {
        for (SwrrPeer<Instance> &peer: peers) {
            peer.normalized_weight /= divisor;
        }
    }

    long double total = 0;
    for (const SwrrPeer<Instance> &peer: peers) {
        total += peer.normalized_weight;
    }
    if (total <= static_cast<long double>(options.max_total_weight)) {
        return;
    }
    const long double scale = static_cast<long double>(options.max_total_weight) / total;
    for (SwrrPeer<Instance> &peer: peers) {
        peer.normalized_weight = std::max<std::int64_t>(1, static_cast<std::int64_t>(peer.normalized_weight * scale));
    }
}

inline std::int64_t swrr_scaled_weight(std::int64_t value, std::int64_t old_base, std::int64_t new_base) noexcept {
    if (old_base <= 0) {
        return new_base;
    }
    const long double ratio =
            static_cast<long double>(value) * static_cast<long double>(new_base) / static_cast<long double>(old_base);
    return static_cast<std::int64_t>(std::llround(ratio));
}

inline bool swrr_excluded(std::span<const std::uint64_t> selection_tokens, std::uint64_t selection_token) noexcept {
    return std::find(selection_tokens.begin(), selection_tokens.end(), selection_token) != selection_tokens.end();
}

template<std::equality_comparable Instance>
bool swrr_circuit_available(const SwrrPeerRuntime &runtime,
                            const typename SmoothWeightedRoundRobin<Instance>::Options &options,
                            typename SmoothWeightedRoundRobin<Instance>::TimePoint now) noexcept {
    return options.max_fails == 0 || runtime.fails < options.max_fails || now - runtime.checked > options.fail_timeout;
}

} // namespace detail

template<std::equality_comparable Instance>
SmoothWeightedRoundRobin<Instance>::Selection::Selection(std::shared_ptr<detail::SwrrGeneration<Instance>> owner,
                                                         std::size_t index, std::uint64_t selection_token) noexcept :
    owner_(std::move(owner)), index_(index), selection_token_(selection_token), pending_(true) {}

template<std::equality_comparable Instance>
SmoothWeightedRoundRobin<Instance>::Selection::Selection(Selection &&other) noexcept :
    owner_(std::move(other.owner_)), index_(other.index_), selection_token_(other.selection_token_),
    pending_(other.pending_) {
    other.pending_ = false;
}

template<std::equality_comparable Instance>
typename SmoothWeightedRoundRobin<Instance>::Selection &
SmoothWeightedRoundRobin<Instance>::Selection::operator=(Selection &&other) noexcept {
    if (this != &other) {
        owner_ = std::move(other.owner_);
        index_ = other.index_;
        selection_token_ = other.selection_token_;
        pending_ = other.pending_;
        other.pending_ = false;
    }
    return *this;
}

template<std::equality_comparable Instance>
bool SmoothWeightedRoundRobin<Instance>::Selection::valid() const noexcept {
    return owner_ && index_ < owner_->peers.size() &&
           owner_->peers[index_].runtime->selection_token == selection_token_;
}

template<std::equality_comparable Instance>
const Instance &SmoothWeightedRoundRobin<Instance>::Selection::instance() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].value.instance;
}

template<std::equality_comparable Instance>
std::uint64_t SmoothWeightedRoundRobin<Instance>::Selection::generation() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->generation;
}

template<std::equality_comparable Instance>
std::uint64_t SmoothWeightedRoundRobin<Instance>::Selection::selection_token() const noexcept {
    FIBER_ASSERT(valid());
    return selection_token_;
}

template<std::equality_comparable Instance>
void SmoothWeightedRoundRobin<Instance>::Selection::report(bool success) noexcept {
    report(success, event::EventLoop::current().now());
}

template<std::equality_comparable Instance>
void SmoothWeightedRoundRobin<Instance>::Selection::report(bool success, TimePoint now) noexcept {
    SmoothWeightedRoundRobin::complete(*this, success, now);
}

template<std::equality_comparable Instance>
SmoothWeightedRoundRobin<Instance>::SmoothWeightedRoundRobin() : SmoothWeightedRoundRobin(Options{}) {}

template<std::equality_comparable Instance>
SmoothWeightedRoundRobin<Instance>::SmoothWeightedRoundRobin(Options options) :
    core_(std::make_shared<Core>(std::move(options))) {
    FIBER_ASSERT(core_->options.max_fails <= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()));
    FIBER_ASSERT(core_->options.fail_timeout > std::chrono::milliseconds::zero());
    FIBER_ASSERT(core_->options.weight_precision > 0);
    FIBER_ASSERT(core_->options.max_normalized_weight > 0);
    FIBER_ASSERT(core_->options.max_total_weight >= core_->options.max_normalized_weight);
}

template<std::equality_comparable Instance>
bool SmoothWeightedRoundRobin<Instance>::update(std::vector<WeightedInstance> instances) {
    std::sort(instances.begin(), instances.end(), [](const WeightedInstance &left, const WeightedInstance &right) {
        return left.selection_token < right.selection_token;
    });

    auto next = std::make_shared<detail::SwrrGeneration<Instance>>();
    next->core = core_;
    next->peers.reserve(instances.size());
    std::uint64_t previous_token = 0;
    for (WeightedInstance &instance: instances) {
        FIBER_ASSERT(instance.selection_token != 0);
        FIBER_ASSERT(instance.selection_token != previous_token);
        FIBER_ASSERT(std::isfinite(instance.weight));
        FIBER_ASSERT(instance.weight > 0.0);
        previous_token = instance.selection_token;
        next->peers.push_back(detail::SwrrPeer<Instance>{
                .value = std::move(instance),
                .runtime = std::make_shared<detail::SwrrPeerRuntime>(),
        });
    }
    detail::swrr_normalize_weights(next->peers, core_->options);

    std::lock_guard guard(core_->mutex);
    const std::shared_ptr<detail::SwrrGeneration<Instance>> current = core_->current;
    if (current && detail::swrr_same_generation(*current, *next)) {
        return false;
    }

    FIBER_ASSERT(!current || current->generation != std::numeric_limits<std::uint64_t>::max());
    next->generation = current ? current->generation + 1 : 1;
    std::size_t old_index = 0;
    std::size_t new_index = 0;
    while (current && old_index < current->peers.size() && new_index < next->peers.size()) {
        detail::SwrrPeer<Instance> &old_peer = current->peers[old_index];
        detail::SwrrPeer<Instance> &new_peer = next->peers[new_index];
        if (old_peer.value.selection_token < new_peer.value.selection_token) {
            old_peer.runtime->retired = true;
            ++old_index;
            continue;
        }
        if (old_peer.value.selection_token > new_peer.value.selection_token) {
            ++new_index;
            continue;
        }

        const std::shared_ptr<detail::SwrrPeerRuntime> runtime = old_peer.runtime;
        runtime->current_weight =
                detail::swrr_scaled_weight(runtime->current_weight, runtime->base_weight, new_peer.normalized_weight);
        runtime->effective_weight = std::clamp(
                detail::swrr_scaled_weight(runtime->effective_weight, runtime->base_weight, new_peer.normalized_weight),
                std::int64_t{0}, new_peer.normalized_weight);
        runtime->base_weight = new_peer.normalized_weight;
        new_peer.runtime = runtime;
        ++old_index;
        ++new_index;
    }
    while (current && old_index < current->peers.size()) {
        current->peers[old_index++].runtime->retired = true;
    }
    for (detail::SwrrPeer<Instance> &peer: next->peers) {
        if (peer.runtime->selection_token == 0) {
            peer.runtime->selection_token = peer.value.selection_token;
            peer.runtime->base_weight = peer.normalized_weight;
            peer.runtime->effective_weight = peer.normalized_weight;
        }
    }
    core_->current = std::move(next);
    return true;
}

template<std::equality_comparable Instance>
std::expected<typename SmoothWeightedRoundRobin<Instance>::Selection, SwrrSelectError>
SmoothWeightedRoundRobin<Instance>::select(std::span<const std::uint64_t> excluded_selection_tokens) noexcept {
    return select(excluded_selection_tokens, event::EventLoop::current().now());
}

template<std::equality_comparable Instance>
std::expected<typename SmoothWeightedRoundRobin<Instance>::Selection, SwrrSelectError>
SmoothWeightedRoundRobin<Instance>::select(std::span<const std::uint64_t> excluded_selection_tokens,
                                           TimePoint now) noexcept {
    std::lock_guard guard(core_->mutex);
    const std::shared_ptr<detail::SwrrGeneration<Instance>> current = core_->current;
    if (!current || current->peers.empty()) {
        return std::unexpected(SwrrSelectError::NoAvailableInstance);
    }

    detail::SwrrPeerRuntime *best = nullptr;
    std::size_t best_index = 0;
    std::int64_t total = 0;
    for (std::size_t i = 0; i < current->peers.size(); ++i) {
        detail::SwrrPeer<Instance> &peer = current->peers[i];
        detail::SwrrPeerRuntime &runtime = *peer.runtime;
        if (runtime.retired || detail::swrr_excluded(excluded_selection_tokens, runtime.selection_token) ||
            !detail::swrr_circuit_available<Instance>(runtime, core_->options, now)) {
            continue;
        }
        runtime.current_weight += runtime.effective_weight;
        total += runtime.effective_weight;
        if (runtime.effective_weight < runtime.base_weight) {
            ++runtime.effective_weight;
        }
        if (!best || runtime.current_weight > best->current_weight) {
            best = &runtime;
            best_index = i;
        }
    }
    if (!best) {
        return std::unexpected(SwrrSelectError::NoAvailableInstance);
    }

    best->current_weight -= total;
    if (now - best->checked > core_->options.fail_timeout) {
        best->checked = now;
    }
    return Selection(std::move(current), best_index, best->selection_token);
}

template<std::equality_comparable Instance>
void SmoothWeightedRoundRobin<Instance>::complete(Selection &selection, bool success, TimePoint now) noexcept {
    if (!selection.pending_ || !selection.valid()) {
        return;
    }
    const std::shared_ptr<detail::SwrrGeneration<Instance>> generation = selection.owner_;
    const std::shared_ptr<Core> core = generation->core;
    std::lock_guard guard(core->mutex);
    detail::SwrrPeerRuntime &runtime = *generation->peers[selection.index_].runtime;
    if (runtime.selection_token != selection.selection_token_) {
        selection.pending_ = false;
        return;
    }

    if (success) {
        if (!runtime.retired && runtime.accessed < runtime.checked) {
            runtime.fails = 0;
        }
    } else if (!runtime.retired) {
        const TimePoint failure_time = std::max(now, runtime.checked);
        if (runtime.fails != std::numeric_limits<std::size_t>::max()) {
            ++runtime.fails;
        }
        runtime.accessed = failure_time;
        runtime.checked = failure_time;
        if (core->options.max_fails != 0) {
            const std::int64_t penalty = std::max<std::int64_t>(
                    1, (runtime.base_weight + static_cast<std::int64_t>(core->options.max_fails) - 1) /
                               static_cast<std::int64_t>(core->options.max_fails));
            runtime.effective_weight = std::max<std::int64_t>(0, runtime.effective_weight - penalty);
        }
    }
    selection.pending_ = false;
}

template<std::equality_comparable Instance>
std::uint64_t SmoothWeightedRoundRobin<Instance>::generation() const noexcept {
    std::lock_guard guard(core_->mutex);
    return core_->current ? core_->current->generation : 0;
}

template<std::equality_comparable Instance>
std::size_t SmoothWeightedRoundRobin<Instance>::configured_instance_count() const noexcept {
    std::lock_guard guard(core_->mutex);
    return core_->current ? core_->current->peers.size() : 0;
}

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_SMOOTH_WEIGHTED_ROUND_ROBIN_IMPL_H
