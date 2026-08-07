#ifndef FIBER_ACCESS_SERVER_SMOOTH_WEIGHTED_ROUND_ROBIN_IMPL_H
#define FIBER_ACCESS_SERVER_SMOOTH_WEIGHTED_ROUND_ROBIN_IMPL_H

#include "SmoothWeightedRoundRobin.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>

namespace fiber::access_server {

template<std::equality_comparable Instance>
struct SmoothWeightedRoundRobin<Instance>::Weighted {
    std::uint64_t selection_token = 0;
    std::int64_t base_weight = 1;
    std::int64_t current_weight = 0;
    std::int64_t effective_weight = 1;
    std::size_t fails = 0;
    TimePoint accessed{};
    TimePoint checked{};
    double configured_weight = 1.0;
    Instance instance;
};

template<std::equality_comparable Instance>
struct SmoothWeightedRoundRobin<Instance>::WeightedStorage {
    std::vector<Weighted> values;
};

template<std::equality_comparable Instance>
struct SmoothWeightedRoundRobin<Instance>::State {
    explicit State(Options value_options) noexcept : options(std::move(value_options)) {}

    Options options;
    std::mutex mutex;
    std::shared_ptr<Weighted> weighted_instances;
    std::unique_ptr<std::uint32_t[]> token_to_index;
    std::size_t weighted_instance_count = 0;
    std::size_t token_to_index_capacity = 0;
    std::uint64_t generation = 0;
};

namespace detail {

inline constexpr std::uint32_t kSwrrEmptyIndex = std::numeric_limits<std::uint32_t>::max();

template<typename Options>
std::int64_t swrr_quantize_weight(double weight, const Options &options) noexcept {
    const long double scaled = static_cast<long double>(weight) * options.weight_precision;
    if (scaled >= static_cast<long double>(options.max_normalized_weight)) {
        return options.max_normalized_weight;
    }
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(scaled)));
}

template<typename Weighted, typename Options>
void swrr_normalize_weights(Weighted *weighted_instances, std::size_t count, const Options &options) noexcept {
    std::int64_t divisor = 0;
    for (std::size_t i = 0; i < count; ++i) {
        Weighted &weighted = weighted_instances[i];
        weighted.base_weight = swrr_quantize_weight(weighted.configured_weight, options);
        divisor = std::gcd(divisor, weighted.base_weight);
    }
    if (divisor > 1) {
        for (std::size_t i = 0; i < count; ++i) {
            weighted_instances[i].base_weight /= divisor;
        }
    }

    long double total = 0;
    for (std::size_t i = 0; i < count; ++i) {
        total += weighted_instances[i].base_weight;
    }
    if (total > static_cast<long double>(options.max_total_weight)) {
        const long double scale = static_cast<long double>(options.max_total_weight) / total;
        for (std::size_t i = 0; i < count; ++i) {
            Weighted &weighted = weighted_instances[i];
            weighted.base_weight = std::max<std::int64_t>(1, static_cast<std::int64_t>(weighted.base_weight * scale));
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        weighted_instances[i].effective_weight = weighted_instances[i].base_weight;
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

inline std::size_t swrr_token_hash(std::uint64_t token) noexcept {
    token ^= token >> 30U;
    token *= 0xbf58476d1ce4e5b9ULL;
    token ^= token >> 27U;
    token *= 0x94d049bb133111ebULL;
    token ^= token >> 31U;
    return static_cast<std::size_t>(token);
}

inline std::size_t swrr_mapping_capacity(std::size_t count) noexcept {
    if (count == 0) {
        return 0;
    }
    FIBER_ASSERT(count < static_cast<std::size_t>(kSwrrEmptyIndex));
    FIBER_ASSERT(count <= std::numeric_limits<std::size_t>::max() / 2);
    const std::size_t required = count * 2;
    std::size_t capacity = 2;
    while (capacity < required) {
        FIBER_ASSERT(capacity <= std::numeric_limits<std::size_t>::max() / 2);
        capacity *= 2;
    }
    return capacity;
}

template<typename Weighted>
void swrr_build_token_mapping(const Weighted *weighted_instances, std::size_t count,
                              std::unique_ptr<std::uint32_t[]> &mapping, std::size_t &capacity) {
    capacity = swrr_mapping_capacity(count);
    if (capacity == 0) {
        mapping.reset();
        return;
    }

    mapping = std::make_unique<std::uint32_t[]>(capacity);
    std::fill_n(mapping.get(), capacity, kSwrrEmptyIndex);
    const std::size_t mask = capacity - 1;
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t slot = swrr_token_hash(weighted_instances[i].selection_token) & mask;
        while (mapping[slot] != kSwrrEmptyIndex) {
            slot = (slot + 1) & mask;
        }
        mapping[slot] = static_cast<std::uint32_t>(i);
    }
}

template<typename Weighted>
std::size_t swrr_find_token(const Weighted *weighted_instances, std::size_t count, const std::uint32_t *mapping,
                            std::size_t capacity, std::uint64_t selection_token) noexcept {
    if (count == 0 || mapping == nullptr || capacity == 0) {
        return count;
    }

    const std::size_t mask = capacity - 1;
    std::size_t slot = swrr_token_hash(selection_token) & mask;
    for (std::size_t probes = 0; probes < capacity; ++probes) {
        const std::uint32_t mapped_index = mapping[slot];
        if (mapped_index == kSwrrEmptyIndex) {
            return count;
        }
        FIBER_ASSERT(static_cast<std::size_t>(mapped_index) < count);
        if (weighted_instances[mapped_index].selection_token == selection_token) {
            return mapped_index;
        }
        slot = (slot + 1) & mask;
    }
    return count;
}

template<typename Weighted>
bool swrr_same_definition(const Weighted *current, std::size_t current_count, const Weighted *next,
                          std::size_t next_count) noexcept {
    if (current_count != next_count) {
        return false;
    }
    for (std::size_t i = 0; i < current_count; ++i) {
        if (current[i].selection_token != next[i].selection_token || current[i].instance != next[i].instance ||
            current[i].configured_weight != next[i].configured_weight ||
            current[i].base_weight != next[i].base_weight) {
            return false;
        }
    }
    return true;
}

inline bool swrr_excluded(std::span<const std::uint64_t> selection_tokens, std::uint64_t selection_token) noexcept {
    return std::find(selection_tokens.begin(), selection_tokens.end(), selection_token) != selection_tokens.end();
}

template<typename Weighted, typename Options, typename TimePoint>
bool swrr_circuit_available(const Weighted &weighted, const Options &options, TimePoint now) noexcept {
    return options.max_fails == 0 || weighted.fails < options.max_fails ||
           now - weighted.checked > options.fail_timeout;
}

} // namespace detail

template<std::equality_comparable Instance>
SmoothWeightedRoundRobin<Instance>::Selection::Selection(std::weak_ptr<State> state,
                                                         std::shared_ptr<Weighted> weighted_instances,
                                                         std::size_t weighted_instance_count, std::size_t index,
                                                         std::uint64_t generation,
                                                         std::uint64_t selection_token) noexcept :
    state_(std::move(state)), weighted_instances_(std::move(weighted_instances)),
    weighted_instance_count_(weighted_instance_count), index_(index), generation_(generation),
    selection_token_(selection_token), pending_(true) {}

template<std::equality_comparable Instance>
SmoothWeightedRoundRobin<Instance>::Selection::Selection(Selection &&other) noexcept :
    state_(std::move(other.state_)), weighted_instances_(std::move(other.weighted_instances_)),
    weighted_instance_count_(other.weighted_instance_count_), index_(other.index_), generation_(other.generation_),
    selection_token_(other.selection_token_), pending_(other.pending_) {
    other.pending_ = false;
}

template<std::equality_comparable Instance>
typename SmoothWeightedRoundRobin<Instance>::Selection &
SmoothWeightedRoundRobin<Instance>::Selection::operator=(Selection &&other) noexcept {
    if (this != &other) {
        state_ = std::move(other.state_);
        weighted_instances_ = std::move(other.weighted_instances_);
        weighted_instance_count_ = other.weighted_instance_count_;
        index_ = other.index_;
        generation_ = other.generation_;
        selection_token_ = other.selection_token_;
        pending_ = other.pending_;
        other.pending_ = false;
    }
    return *this;
}

template<std::equality_comparable Instance>
bool SmoothWeightedRoundRobin<Instance>::Selection::valid() const noexcept {
    return weighted_instances_ && index_ < weighted_instance_count_ &&
           weighted_instances_.get()[index_].selection_token == selection_token_;
}

template<std::equality_comparable Instance>
const Instance &SmoothWeightedRoundRobin<Instance>::Selection::instance() const noexcept {
    FIBER_ASSERT(valid());
    return weighted_instances_.get()[index_].instance;
}

template<std::equality_comparable Instance>
std::uint64_t SmoothWeightedRoundRobin<Instance>::Selection::generation() const noexcept {
    FIBER_ASSERT(valid());
    return generation_;
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
    state_(std::make_shared<State>(std::move(options))) {
    FIBER_ASSERT(state_->options.max_fails <= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()));
    FIBER_ASSERT(state_->options.fail_timeout > std::chrono::milliseconds::zero());
    FIBER_ASSERT(state_->options.weight_precision > 0);
    FIBER_ASSERT(state_->options.max_normalized_weight > 0);
    FIBER_ASSERT(state_->options.max_total_weight >= state_->options.max_normalized_weight);
}

template<std::equality_comparable Instance>
bool SmoothWeightedRoundRobin<Instance>::update(std::vector<WeightedInstance> instances) {
    std::sort(instances.begin(), instances.end(), [](const WeightedInstance &left, const WeightedInstance &right) {
        return left.selection_token < right.selection_token;
    });

    auto next_storage = std::make_shared<WeightedStorage>();
    next_storage->values.reserve(instances.size());
    std::uint64_t previous_token = 0;
    for (WeightedInstance &instance: instances) {
        FIBER_ASSERT(instance.selection_token != 0);
        FIBER_ASSERT(instance.selection_token != previous_token);
        FIBER_ASSERT(std::isfinite(instance.weight));
        FIBER_ASSERT(instance.weight > 0.0);
        previous_token = instance.selection_token;
        next_storage->values.push_back(Weighted{
                .selection_token = instance.selection_token,
                .configured_weight = instance.weight,
                .instance = std::move(instance.instance),
        });
    }

    Weighted *next_values = next_storage->values.data();
    const std::size_t next_count = next_storage->values.size();
    detail::swrr_normalize_weights(next_values, next_count, state_->options);
    std::shared_ptr<Weighted> next_weighted_instances;
    if (next_count != 0) {
        next_weighted_instances = std::shared_ptr<Weighted>(next_storage, next_values);
    }
    std::unique_ptr<std::uint32_t[]> next_token_to_index;
    std::size_t next_token_to_index_capacity = 0;
    detail::swrr_build_token_mapping(next_values, next_count, next_token_to_index, next_token_to_index_capacity);

    std::lock_guard guard(state_->mutex);
    Weighted *current_values = state_->weighted_instances.get();
    if (state_->generation != 0 &&
        detail::swrr_same_definition(current_values, state_->weighted_instance_count, next_values, next_count)) {
        return false;
    }

    FIBER_ASSERT(state_->generation != std::numeric_limits<std::uint64_t>::max());
    for (std::size_t i = 0; i < next_count; ++i) {
        Weighted &next = next_values[i];
        const std::size_t old_index =
                detail::swrr_find_token(current_values, state_->weighted_instance_count, state_->token_to_index.get(),
                                        state_->token_to_index_capacity, next.selection_token);
        if (old_index == state_->weighted_instance_count) {
            continue;
        }

        const Weighted &current = current_values[old_index];
        next.current_weight = detail::swrr_scaled_weight(current.current_weight, current.base_weight, next.base_weight);
        next.effective_weight =
                std::clamp(detail::swrr_scaled_weight(current.effective_weight, current.base_weight, next.base_weight),
                           std::int64_t{0}, next.base_weight);
        next.fails = current.fails;
        next.accessed = current.accessed;
        next.checked = current.checked;
    }

    state_->weighted_instances = std::move(next_weighted_instances);
    state_->weighted_instance_count = next_count;
    state_->token_to_index = std::move(next_token_to_index);
    state_->token_to_index_capacity = next_token_to_index_capacity;
    ++state_->generation;
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
    std::lock_guard guard(state_->mutex);
    Weighted *weighted_instances = state_->weighted_instances.get();
    if (state_->weighted_instance_count == 0) {
        return std::unexpected(SwrrSelectError::NoConfiguredInstance);
    }

    Weighted *best = nullptr;
    std::size_t best_index = 0;
    std::int64_t total = 0;
    for (std::size_t i = 0; i < state_->weighted_instance_count; ++i) {
        Weighted &weighted = weighted_instances[i];
        if (detail::swrr_excluded(excluded_selection_tokens, weighted.selection_token) ||
            !detail::swrr_circuit_available(weighted, state_->options, now)) {
            continue;
        }
        weighted.current_weight += weighted.effective_weight;
        total += weighted.effective_weight;
        if (weighted.effective_weight < weighted.base_weight) {
            ++weighted.effective_weight;
        }
        if (!best || weighted.current_weight > best->current_weight) {
            best = &weighted;
            best_index = i;
        }
    }
    if (!best) {
        return std::unexpected(SwrrSelectError::NoAvailableInstance);
    }

    best->current_weight -= total;
    if (now - best->checked > state_->options.fail_timeout) {
        best->checked = now;
    }
    return Selection(state_, state_->weighted_instances, state_->weighted_instance_count, best_index,
                     state_->generation, best->selection_token);
}

template<std::equality_comparable Instance>
void SmoothWeightedRoundRobin<Instance>::complete(Selection &selection, bool success, TimePoint now) noexcept {
    if (!selection.pending_ || !selection.valid()) {
        return;
    }

    const std::shared_ptr<State> state = selection.state_.lock();
    if (!state) {
        selection.pending_ = false;
        return;
    }

    std::lock_guard guard(state->mutex);
    Weighted *current_values = state->weighted_instances.get();
    std::size_t current_index = state->weighted_instance_count;
    if (selection.generation_ == state->generation && selection.index_ < state->weighted_instance_count &&
        current_values[selection.index_].selection_token == selection.selection_token_) {
        current_index = selection.index_;
    } else {
        current_index =
                detail::swrr_find_token(current_values, state->weighted_instance_count, state->token_to_index.get(),
                                        state->token_to_index_capacity, selection.selection_token_);
    }
    if (current_index == state->weighted_instance_count) {
        selection.pending_ = false;
        return;
    }

    Weighted &weighted = current_values[current_index];
    if (success) {
        if (weighted.accessed < weighted.checked) {
            weighted.fails = 0;
        }
    } else {
        const TimePoint failure_time = std::max(now, weighted.checked);
        if (weighted.fails != std::numeric_limits<std::size_t>::max()) {
            ++weighted.fails;
        }
        weighted.accessed = failure_time;
        weighted.checked = failure_time;
        if (state->options.max_fails != 0) {
            const std::int64_t penalty = std::max<std::int64_t>(
                    1, (weighted.base_weight + static_cast<std::int64_t>(state->options.max_fails) - 1) /
                               static_cast<std::int64_t>(state->options.max_fails));
            weighted.effective_weight = std::max<std::int64_t>(0, weighted.effective_weight - penalty);
        }
    }
    selection.pending_ = false;
}

template<std::equality_comparable Instance>
std::uint64_t SmoothWeightedRoundRobin<Instance>::generation() const noexcept {
    std::lock_guard guard(state_->mutex);
    return state_->generation;
}

template<std::equality_comparable Instance>
std::size_t SmoothWeightedRoundRobin<Instance>::configured_instance_count() const noexcept {
    std::lock_guard guard(state_->mutex);
    return state_->weighted_instance_count;
}

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_SMOOTH_WEIGHTED_ROUND_ROBIN_IMPL_H
