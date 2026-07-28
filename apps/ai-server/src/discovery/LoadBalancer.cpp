#include "LoadBalancer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <utility>

#include <common/Assert.h>
#include <event/EventLoop.h>

namespace fiber::ai_server {

struct LoadBalancer::Core {
    explicit Core(Options value) noexcept : options(std::move(value)) {}

    Options options;
    std::mutex mutex;
    std::uint64_t next_peer_epoch = 0;
    bool shutdown = false;
    std::atomic<std::uint64_t> selections{0};
    std::atomic<std::uint64_t> unavailable{0};
    std::atomic<std::uint64_t> success_reports{0};
    std::atomic<std::uint64_t> failure_reports{0};
    std::atomic<std::uint64_t> neutral_reports{0};
    std::atomic<std::uint64_t> circuit_opens{0};
};

namespace detail {

struct PeerRuntime {
    std::int64_t base_weight = 1;
    std::int64_t effective_weight = 1;
    std::int64_t current_weight = 0;
    std::size_t fails = 0;
    LoadBalancer::TimePoint accessed{};
    LoadBalancer::TimePoint checked{};
    std::size_t in_flight = 0;
    std::uint64_t peer_epoch = 0;
    bool retired = false;
};

struct PeerEntry {
    DiscoveredInstance instance;
    std::int64_t normalized_weight = 1;
    std::shared_ptr<PeerRuntime> runtime;
};

struct RoundRobin {
    std::shared_ptr<LoadBalancer::Core> core;
    std::uint64_t generation = 0;
    std::string service_name;
    std::string group;
    std::string checksum;
    std::int64_t last_ref_time = 0;
    std::vector<PeerEntry> peers;
};

} // namespace detail

namespace {

using PeerEntry = detail::PeerEntry;
using PeerRuntime = detail::PeerRuntime;
using RoundRobin = detail::RoundRobin;

std::uint64_t mix_rendezvous_hash(std::uint64_t key, std::uint64_t peer_hash) noexcept {
    std::uint64_t value = key ^ (peer_hash + 0x9e3779b97f4a7c15ULL);
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value;
}

double negative_log_hash_uniform(std::uint64_t hash) noexcept {
    // The hash midpoint is (2 * (hash >> 11) + 1) / 2^54.  Reduce it to
    // mantissa * 2^-exponent with mantissa in [0.5, 1), then evaluate
    // -log(mantissa) through the rapidly converging atanh series.  This keeps
    // weighted rendezvous independent of the platform libm symbol version.
    constexpr double kLn2 = 0x1.62e42fefa39efp-1;
    constexpr std::array<double, 16> kReciprocalOdd{
            1.0,        1.0 / 3.0,  1.0 / 5.0,  1.0 / 7.0,  1.0 / 9.0,  1.0 / 11.0, 1.0 / 13.0, 1.0 / 15.0,
            1.0 / 17.0, 1.0 / 19.0, 1.0 / 21.0, 1.0 / 23.0, 1.0 / 25.0, 1.0 / 27.0, 1.0 / 29.0, 1.0 / 31.0,
    };

    const std::uint64_t midpoint = ((hash >> 11U) << 1U) | 1U;
    const unsigned highest_bit = 63U - static_cast<unsigned>(std::countl_zero(midpoint));
    const unsigned exponent = 53U - highest_bit;
    const double mantissa = static_cast<double>(midpoint) / static_cast<double>(std::uint64_t{1} << (highest_bit + 1U));
    const double z = (1.0 - mantissa) / (1.0 + mantissa);
    const double z_squared = z * z;

    double polynomial = kReciprocalOdd.back();
    for (std::size_t i = kReciprocalOdd.size() - 1; i != 0; --i) {
        polynomial = kReciprocalOdd[i - 1] + z_squared * polynomial;
    }
    return static_cast<double>(exponent) * kLn2 + 2.0 * z * polynomial;
}

double weighted_rendezvous_cost(std::uint64_t hash, std::int64_t weight) noexcept {
    return negative_log_hash_uniform(hash) / static_cast<double>(weight);
}

bool excluded_peer(std::span<const std::uint64_t> excluded_peer_ids, std::uint64_t peer_id) noexcept {
    return std::find(excluded_peer_ids.begin(), excluded_peer_ids.end(), peer_id) != excluded_peer_ids.end();
}

int compare_address(const net::SocketAddress &left, const net::SocketAddress &right) noexcept {
    if (left.family() != right.family()) {
        return left.family() < right.family() ? -1 : 1;
    }
    if (left.ip().is_v4()) {
        const auto left_bytes = left.ip().v4_bytes();
        const auto right_bytes = right.ip().v4_bytes();
        if (left_bytes != right_bytes) {
            return left_bytes < right_bytes ? -1 : 1;
        }
    } else {
        if (left.ip().v6_bytes() != right.ip().v6_bytes()) {
            return left.ip().v6_bytes() < right.ip().v6_bytes() ? -1 : 1;
        }
        if (left.ip().scope_id() != right.ip().scope_id()) {
            return left.ip().scope_id() < right.ip().scope_id() ? -1 : 1;
        }
    }
    if (left.port() != right.port()) {
        return left.port() < right.port() ? -1 : 1;
    }
    return 0;
}

int compare_entry(const PeerEntry &left, const PeerEntry &right) noexcept {
    const auto left_scheme = left.instance.connection_key.scheme();
    const auto right_scheme = right.instance.connection_key.scheme();
    if (left_scheme != right_scheme) {
        return left_scheme < right_scheme ? -1 : 1;
    }
    return compare_address(left.instance.address, right.instance.address);
}

bool same_endpoint(const PeerEntry &left, const PeerEntry &right) noexcept { return compare_entry(left, right) == 0; }

bool same_instance_definition(const PeerEntry &left, const PeerEntry &right) noexcept {
    return same_endpoint(left, right) && left.instance.instance_id == right.instance.instance_id &&
           left.instance.host_header == right.instance.host_header &&
           left.instance.cluster_name == right.instance.cluster_name && left.instance.weight == right.instance.weight &&
           left.normalized_weight == right.normalized_weight;
}

std::int64_t quantize_weight(double weight, const LoadBalancer::Options &options) noexcept {
    if (!std::isfinite(weight) || weight <= 0.0) {
        return 0;
    }
    const long double scaled = static_cast<long double>(weight) * options.weight_precision;
    if (scaled >= static_cast<long double>(options.max_normalized_weight)) {
        return options.max_normalized_weight;
    }
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(scaled)));
}

void normalize_weights(std::vector<PeerEntry> &peers, const LoadBalancer::Options &options) noexcept {
    std::int64_t divisor = 0;
    for (PeerEntry &peer: peers) {
        peer.normalized_weight = quantize_weight(peer.instance.weight, options);
        divisor = std::gcd(divisor, peer.normalized_weight);
    }
    if (divisor > 1) {
        for (PeerEntry &peer: peers) {
            peer.normalized_weight /= divisor;
        }
    }

    long double total = 0;
    for (const PeerEntry &peer: peers) {
        total += peer.normalized_weight;
    }
    if (total <= static_cast<long double>(options.max_total_weight)) {
        return;
    }
    const long double scale = static_cast<long double>(options.max_total_weight) / total;
    for (PeerEntry &entry: peers) {
        entry.normalized_weight = std::max<std::int64_t>(1, static_cast<std::int64_t>(entry.normalized_weight * scale));
    }
}

bool same_round_robin(const RoundRobin &current, const RoundRobin &next) noexcept {
    if (current.service_name != next.service_name || current.group != next.group) {
        return false;
    }
    if (!current.checksum.empty() || !next.checksum.empty()) {
        return !current.checksum.empty() && current.checksum == next.checksum;
    }
    if (current.peers.size() != next.peers.size()) {
        return false;
    }
    for (std::size_t i = 0; i < current.peers.size(); ++i) {
        if (!same_instance_definition(current.peers[i], next.peers[i])) {
            return false;
        }
    }
    return true;
}

std::int64_t scaled_effective_weight(std::int64_t effective, std::int64_t old_base, std::int64_t new_base) noexcept {
    if (old_base <= 0) {
        return new_base;
    }
    const long double ratio = static_cast<long double>(effective) * static_cast<long double>(new_base) /
                              static_cast<long double>(old_base);
    return std::clamp<std::int64_t>(static_cast<std::int64_t>(std::llround(ratio)), 0, new_base);
}

} // namespace

LoadBalancer::Instance::Instance(std::shared_ptr<detail::RoundRobin> owner, std::size_t index,
                                 std::uint64_t peer_epoch) noexcept :
    owner_(std::move(owner)), index_(index), peer_epoch_(peer_epoch), pending_(true) {}

LoadBalancer::Instance::~Instance() { release_neutral(); }

LoadBalancer::Instance::Instance(Instance &&other) noexcept :
    owner_(std::move(other.owner_)), index_(other.index_), peer_epoch_(other.peer_epoch_), pending_(other.pending_) {
    other.pending_ = false;
}

LoadBalancer::Instance &LoadBalancer::Instance::operator=(Instance &&other) noexcept {
    if (this != &other) {
        release_neutral();
        owner_ = std::move(other.owner_);
        index_ = other.index_;
        peer_epoch_ = other.peer_epoch_;
        pending_ = other.pending_;
        other.pending_ = false;
    }
    return *this;
}

bool LoadBalancer::Instance::valid() const noexcept {
    return pending_ && owner_ && index_ < owner_->peers.size() &&
           owner_->peers[index_].runtime->peer_epoch == peer_epoch_;
}

const net::SocketAddress &LoadBalancer::Instance::address() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].instance.address;
}

const http::Http1ConnectionGroupKey &LoadBalancer::Instance::connection_key() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].instance.connection_key;
}

std::string_view LoadBalancer::Instance::host_header() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].instance.host_header;
}

std::string_view LoadBalancer::Instance::instance_id() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].instance.instance_id;
}

std::string_view LoadBalancer::Instance::cluster_name() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].instance.cluster_name;
}

std::string_view LoadBalancer::Instance::service_name() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->service_name;
}

double LoadBalancer::Instance::configured_weight() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].instance.weight;
}

std::int64_t LoadBalancer::Instance::normalized_weight() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].normalized_weight;
}

std::uint64_t LoadBalancer::Instance::generation() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->generation;
}

std::uint64_t LoadBalancer::Instance::peer_id() const noexcept {
    FIBER_ASSERT(valid());
    return peer_epoch_;
}

void LoadBalancer::Instance::release_neutral() noexcept {
    LoadBalancer::complete_instance(*this, InstanceReportOutcome::Neutral, TimePoint{});
}

LoadBalancer::LoadBalancer() : LoadBalancer(Options{}) {}

LoadBalancer::LoadBalancer(Options options) : core_(std::make_shared<Core>(std::move(options))) {
    FIBER_ASSERT(core_->options.max_fails <= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()));
    FIBER_ASSERT(core_->options.fail_timeout > std::chrono::milliseconds::zero());
    FIBER_ASSERT(core_->options.weight_precision > 0);
    FIBER_ASSERT(core_->options.max_normalized_weight > 0);
    FIBER_ASSERT(core_->options.max_total_weight >= core_->options.max_normalized_weight);
}

LoadBalancer::~LoadBalancer() { shutdown(); }

std::shared_ptr<RoundRobin> LoadBalancer::load_current() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    return current_.load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(&current_, std::memory_order_acquire);
#endif
}

void LoadBalancer::store_current(std::shared_ptr<RoundRobin> current) noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    current_.store(std::move(current), std::memory_order_release);
#else
    std::atomic_store_explicit(&current_, std::move(current), std::memory_order_release);
#endif
}

std::expected<LoadBalancer::Instance, LoadBalanceError> LoadBalancer::load_balance() noexcept {
    return load_balance(event::EventLoop::current().now());
}

std::expected<LoadBalancer::Instance, LoadBalanceError> LoadBalancer::load_balance(TimePoint now) noexcept {
    std::lock_guard guard(core_->mutex);
    if (core_->shutdown) {
        return std::unexpected(LoadBalanceError::Shutdown);
    }
    std::shared_ptr<RoundRobin> current = load_current();
    if (!current) {
        return std::unexpected(LoadBalanceError::Uninitialized);
    }
    if (current->peers.empty()) {
        core_->unavailable.fetch_add(1, std::memory_order_relaxed);
        return std::unexpected(LoadBalanceError::NoAvailableInstance);
    }

    PeerRuntime *best = nullptr;
    std::size_t best_index = 0;
    std::int64_t total = 0;
    for (std::size_t i = 0; i < current->peers.size(); ++i) {
        PeerRuntime &runtime = *current->peers[i].runtime;
        if (runtime.retired) {
            continue;
        }
        if (core_->options.max_fails != 0 && runtime.fails >= core_->options.max_fails &&
            now - runtime.checked <= core_->options.fail_timeout) {
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
        if (core_->options.fail_open_when_single && current->peers.size() == 1) {
            best = current->peers[0].runtime.get();
            best_index = 0;
        } else {
            core_->unavailable.fetch_add(1, std::memory_order_relaxed);
            return std::unexpected(LoadBalanceError::NoAvailableInstance);
        }
    }

    best->current_weight -= total;
    if (now - best->checked > core_->options.fail_timeout) {
        best->checked = now;
    }
    FIBER_ASSERT(best->in_flight != std::numeric_limits<std::size_t>::max());
    ++best->in_flight;
    core_->selections.fetch_add(1, std::memory_order_relaxed);
    return Instance(std::move(current), best_index, best->peer_epoch);
}

std::expected<LoadBalancer::Instance, LoadBalanceError>
LoadBalancer::load_balance(std::uint64_t key, std::span<const std::uint64_t> excluded_peer_ids) noexcept {
    return load_balance(key, excluded_peer_ids, event::EventLoop::current().now());
}

std::expected<LoadBalancer::Instance, LoadBalanceError>
LoadBalancer::load_balance(std::uint64_t key, std::span<const std::uint64_t> excluded_peer_ids,
                           TimePoint now) noexcept {
    std::lock_guard guard(core_->mutex);
    if (core_->shutdown) {
        return std::unexpected(LoadBalanceError::Shutdown);
    }
    std::shared_ptr<RoundRobin> current = load_current();
    if (!current) {
        return std::unexpected(LoadBalanceError::Uninitialized);
    }
    if (current->peers.empty()) {
        core_->unavailable.fetch_add(1, std::memory_order_relaxed);
        return std::unexpected(LoadBalanceError::NoAvailableInstance);
    }

    PeerRuntime *best = nullptr;
    std::size_t best_index = 0;
    double best_cost = 0;
    for (std::size_t i = 0; i < current->peers.size(); ++i) {
        PeerEntry &peer = current->peers[i];
        PeerRuntime &runtime = *peer.runtime;
        if (runtime.retired) {
            continue;
        }
        if (core_->options.max_fails != 0 && runtime.fails >= core_->options.max_fails &&
            now - runtime.checked <= core_->options.fail_timeout) {
            continue;
        }
        if (runtime.effective_weight < runtime.base_weight) {
            ++runtime.effective_weight;
        }
        if (excluded_peer(excluded_peer_ids, runtime.peer_epoch)) {
            continue;
        }

        const double cost = weighted_rendezvous_cost(mix_rendezvous_hash(key, peer.instance.connection_key.hash()),
                                                     peer.normalized_weight);
        if (!best || cost < best_cost || (cost == best_cost && compare_entry(peer, current->peers[best_index]) < 0)) {
            best = &runtime;
            best_index = i;
            best_cost = cost;
        }
    }

    if (!best) {
        if (core_->options.fail_open_when_single && current->peers.size() == 1 && !current->peers[0].runtime->retired &&
            !excluded_peer(excluded_peer_ids, current->peers[0].runtime->peer_epoch)) {
            best = current->peers[0].runtime.get();
            best_index = 0;
        } else {
            core_->unavailable.fetch_add(1, std::memory_order_relaxed);
            return std::unexpected(LoadBalanceError::NoAvailableInstance);
        }
    }

    if (now - best->checked > core_->options.fail_timeout) {
        best->checked = now;
    }
    FIBER_ASSERT(best->in_flight != std::numeric_limits<std::size_t>::max());
    ++best->in_flight;
    core_->selections.fetch_add(1, std::memory_order_relaxed);
    return Instance(std::move(current), best_index, best->peer_epoch);
}

void LoadBalancer::report(Instance &&instance, InstanceReportOutcome outcome) noexcept {
    report(std::move(instance), outcome, event::EventLoop::current().now());
}

void LoadBalancer::report(Instance &instance, bool success) noexcept {
    report(instance, success, event::EventLoop::current().now());
}

void LoadBalancer::report(Instance &instance, bool success, TimePoint now) noexcept {
    if (!instance.valid()) {
        return;
    }
    FIBER_ASSERT(instance.owner_->core == core_);
    complete_instance(instance, success ? InstanceReportOutcome::Success : InstanceReportOutcome::Failure, now);
}

void LoadBalancer::report(Instance &&instance, InstanceReportOutcome outcome, TimePoint now) noexcept {
    if (!instance.valid()) {
        return;
    }
    FIBER_ASSERT(instance.owner_->core == core_);
    complete_instance(instance, outcome, now);
}

void LoadBalancer::complete_instance(Instance &instance, InstanceReportOutcome outcome, TimePoint now) noexcept {
    if (!instance.pending_ || !instance.owner_) {
        return;
    }
    const std::shared_ptr<RoundRobin> owner = instance.owner_;
    const std::shared_ptr<Core> core = owner->core;
    std::lock_guard guard(core->mutex);
    if (instance.index_ >= owner->peers.size()) {
        instance.pending_ = false;
        instance.owner_.reset();
        return;
    }

    PeerEntry &entry = owner->peers[instance.index_];
    PeerRuntime &runtime = *entry.runtime;
    if (runtime.peer_epoch != instance.peer_epoch_) {
        instance.pending_ = false;
        instance.owner_.reset();
        return;
    }
    FIBER_ASSERT(runtime.in_flight > 0);
    --runtime.in_flight;

    switch (outcome) {
        case InstanceReportOutcome::Success:
            core->success_reports.fetch_add(1, std::memory_order_relaxed);
            if (!runtime.retired && runtime.accessed < runtime.checked) {
                runtime.fails = 0;
            }
            break;
        case InstanceReportOutcome::Failure:
            core->failure_reports.fetch_add(1, std::memory_order_relaxed);
            if (!runtime.retired) {
                // Event loops cache now independently; never let a report from a slower worker move peer time back.
                const TimePoint failure_time = std::max(now, runtime.checked);
                const bool was_open = core->options.max_fails != 0 && runtime.fails >= core->options.max_fails &&
                                      failure_time - runtime.checked <= core->options.fail_timeout;
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
                    if (!was_open && runtime.fails >= core->options.max_fails) {
                        core->circuit_opens.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            break;
        case InstanceReportOutcome::Neutral:
            core->neutral_reports.fetch_add(1, std::memory_order_relaxed);
            break;
    }

    instance.pending_ = false;
    instance.owner_.reset();
}

LoadBalancerUpdateResult LoadBalancer::update_instances(DiscoveredService update) {
    auto next = std::make_shared<RoundRobin>();
    next->core = core_;
    next->service_name = std::move(update.service_name);
    next->group = std::move(update.group);
    next->checksum = std::move(update.checksum);
    next->last_ref_time = update.last_ref_time;
    next->peers.reserve(update.instances.size());
    for (DiscoveredInstance &instance: update.instances) {
        if (!std::isfinite(instance.weight) || instance.weight <= 0.0 || instance.address.port() == 0) {
            continue;
        }
        auto runtime = std::make_shared<PeerRuntime>();
        next->peers.push_back(PeerEntry{
                .instance = std::move(instance),
                .runtime = std::move(runtime),
        });
    }
    std::sort(next->peers.begin(), next->peers.end(), [](const PeerEntry &left, const PeerEntry &right) {
        const int endpoint = compare_entry(left, right);
        if (endpoint != 0) {
            return endpoint < 0;
        }
        if (left.instance.instance_id != right.instance.instance_id) {
            return left.instance.instance_id < right.instance.instance_id;
        }
        if (left.instance.cluster_name != right.instance.cluster_name) {
            return left.instance.cluster_name < right.instance.cluster_name;
        }
        if (left.instance.host_header != right.instance.host_header) {
            return left.instance.host_header < right.instance.host_header;
        }
        return left.instance.weight < right.instance.weight;
    });
    next->peers.erase(
            std::unique(next->peers.begin(), next->peers.end(),
                        [](const PeerEntry &left, const PeerEntry &right) { return same_endpoint(left, right); }),
            next->peers.end());
    normalize_weights(next->peers, core_->options);

    std::lock_guard guard(core_->mutex);
    if (core_->shutdown) {
        return LoadBalancerUpdateResult::Unchanged;
    }
    std::shared_ptr<RoundRobin> current = load_current();
    if (current && same_round_robin(*current, *next)) {
        return LoadBalancerUpdateResult::Unchanged;
    }

    FIBER_ASSERT(!current || current->generation != std::numeric_limits<std::uint64_t>::max());
    next->generation = current ? current->generation + 1 : 1;
    std::size_t old_index = 0;
    std::size_t new_index = 0;
    while (current && old_index < current->peers.size() && new_index < next->peers.size()) {
        PeerEntry &old_peer = current->peers[old_index];
        PeerEntry &new_peer = next->peers[new_index];
        const int comparison = compare_entry(old_peer, new_peer);
        if (comparison < 0) {
            old_peer.runtime->retired = true;
            ++old_index;
            continue;
        }
        if (comparison > 0) {
            ++new_index;
            continue;
        }

        const std::shared_ptr<PeerRuntime> runtime = old_peer.runtime;
        if (runtime->base_weight != new_peer.normalized_weight) {
            runtime->effective_weight = scaled_effective_weight(runtime->effective_weight, runtime->base_weight,
                                                                new_peer.normalized_weight);
            runtime->current_weight = 0;
            runtime->base_weight = new_peer.normalized_weight;
        }
        runtime->retired = false;
        new_peer.runtime = runtime;
        ++old_index;
        ++new_index;
    }
    while (current && old_index < current->peers.size()) {
        current->peers[old_index++].runtime->retired = true;
    }
    for (PeerEntry &peer: next->peers) {
        PeerRuntime &runtime = *peer.runtime;
        if (runtime.peer_epoch == 0) {
            FIBER_ASSERT(core_->next_peer_epoch != std::numeric_limits<std::uint64_t>::max());
            runtime.peer_epoch = ++core_->next_peer_epoch;
            runtime.base_weight = peer.normalized_weight;
            runtime.effective_weight = peer.normalized_weight;
            runtime.current_weight = 0;
            runtime.retired = false;
        }
    }

    store_current(std::move(next));
    return LoadBalancerUpdateResult::Applied;
}

void LoadBalancer::shutdown() noexcept {
    if (!core_) {
        return;
    }
    std::lock_guard guard(core_->mutex);
    core_->shutdown = true;
}

bool LoadBalancer::initialized() const noexcept { return load_current() != nullptr; }

std::uint64_t LoadBalancer::generation() const noexcept {
    const std::shared_ptr<RoundRobin> current = load_current();
    return current ? current->generation : 0;
}

std::size_t LoadBalancer::configured_instance_count() const noexcept {
    const std::shared_ptr<RoundRobin> current = load_current();
    return current ? current->peers.size() : 0;
}

LoadBalancerStats LoadBalancer::stats() const noexcept {
    const std::shared_ptr<RoundRobin> current = load_current();
    return LoadBalancerStats{
            .generation = current ? current->generation : 0,
            .configured_instances = current ? current->peers.size() : 0,
            .selections = core_->selections.load(std::memory_order_relaxed),
            .unavailable = core_->unavailable.load(std::memory_order_relaxed),
            .success_reports = core_->success_reports.load(std::memory_order_relaxed),
            .failure_reports = core_->failure_reports.load(std::memory_order_relaxed),
            .neutral_reports = core_->neutral_reports.load(std::memory_order_relaxed),
            .circuit_opens = core_->circuit_opens.load(std::memory_order_relaxed),
    };
}

} // namespace fiber::ai_server
