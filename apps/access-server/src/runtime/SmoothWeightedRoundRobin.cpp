#include "SmoothWeightedRoundRobin.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <mutex>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include <common/Assert.h>
#include <event/EventLoop.h>
#include <fiber/nacos/NamingService.h>

namespace fiber::access_server {

struct SmoothWeightedRoundRobin::Core {
    explicit Core(Options value_options) noexcept : options(std::move(value_options)) {}

    Options options;
    std::mutex mutex;
    std::shared_ptr<detail::SwrrGeneration> current;
    std::uint64_t next_peer_epoch = 0;
};

namespace detail {

struct SwrrEndpoint {
    std::string instance_id;
    std::string host;
    std::optional<net::IpAddress> ip_address;
    std::uint16_t port = 0;
    std::string authority;
    double weight = 1.0;
    std::string cluster_name;
};

struct SwrrPeerRuntime {
    std::int64_t base_weight = 1;
    std::int64_t effective_weight = 1;
    std::int64_t current_weight = 0;
    std::size_t fails = 0;
    SmoothWeightedRoundRobin::TimePoint accessed{};
    SmoothWeightedRoundRobin::TimePoint checked{};
    std::uint64_t peer_epoch = 0;
    bool retired = false;
};

struct SwrrPeer {
    SwrrEndpoint endpoint;
    std::int64_t normalized_weight = 1;
    std::shared_ptr<SwrrPeerRuntime> runtime;
};

struct SwrrGeneration {
    std::shared_ptr<SmoothWeightedRoundRobin::Core> core;
    std::uint64_t generation = 0;
    std::string checksum;
    std::vector<SwrrPeer> peers;
};

} // namespace detail

namespace {

using Endpoint = detail::SwrrEndpoint;
using Generation = detail::SwrrGeneration;
using Peer = detail::SwrrPeer;
using PeerRuntime = detail::SwrrPeerRuntime;

std::string make_authority(std::string_view host, std::uint16_t port, const std::optional<net::IpAddress> &ip) {
    std::array<char, 5> port_text{};
    const auto converted = std::to_chars(port_text.data(), port_text.data() + port_text.size(), port);
    std::string result;
    result.reserve(host.size() + 8);
    if (ip && ip->is_v6()) {
        result.push_back('[');
        result.append(host);
        result.push_back(']');
    } else {
        result.append(host);
    }
    result.push_back(':');
    result.append(port_text.data(), converted.ptr);
    return result;
}

int compare_ip(const net::IpAddress &left, const net::IpAddress &right) noexcept {
    if (left.family() != right.family()) {
        return left.family() < right.family() ? -1 : 1;
    }
    if (left.is_v4()) {
        if (left.v4_bytes() != right.v4_bytes()) {
            return left.v4_bytes() < right.v4_bytes() ? -1 : 1;
        }
    } else {
        if (left.v6_bytes() != right.v6_bytes()) {
            return left.v6_bytes() < right.v6_bytes() ? -1 : 1;
        }
        if (left.scope_id() != right.scope_id()) {
            return left.scope_id() < right.scope_id() ? -1 : 1;
        }
    }
    return 0;
}

int compare_peer(const Peer &left, const Peer &right) noexcept {
    if (left.endpoint.ip_address.has_value() != right.endpoint.ip_address.has_value()) {
        return left.endpoint.ip_address ? 1 : -1;
    }
    if (left.endpoint.ip_address) {
        const int compared = compare_ip(*left.endpoint.ip_address, *right.endpoint.ip_address);
        if (compared != 0) {
            return compared;
        }
    } else if (left.endpoint.host != right.endpoint.host) {
        return left.endpoint.host < right.endpoint.host ? -1 : 1;
    }
    if (left.endpoint.port != right.endpoint.port) {
        return left.endpoint.port < right.endpoint.port ? -1 : 1;
    }
    if (left.endpoint.cluster_name != right.endpoint.cluster_name) {
        return left.endpoint.cluster_name < right.endpoint.cluster_name ? -1 : 1;
    }
    return 0;
}

bool same_definition(const Peer &left, const Peer &right) noexcept {
    return compare_peer(left, right) == 0 && left.endpoint.instance_id == right.endpoint.instance_id &&
           left.endpoint.authority == right.endpoint.authority && left.endpoint.weight == right.endpoint.weight &&
           left.normalized_weight == right.normalized_weight;
}

bool same_generation(const Generation &current, const Generation &next) noexcept {
    if (!current.checksum.empty() || !next.checksum.empty()) {
        return !current.checksum.empty() && current.checksum == next.checksum;
    }
    if (current.peers.size() != next.peers.size()) {
        return false;
    }
    for (std::size_t i = 0; i < current.peers.size(); ++i) {
        if (!same_definition(current.peers[i], next.peers[i])) {
            return false;
        }
    }
    return true;
}

std::int64_t quantize_weight(double weight, const SmoothWeightedRoundRobin::Options &options) noexcept {
    const long double scaled = static_cast<long double>(weight) * options.weight_precision;
    if (scaled >= static_cast<long double>(options.max_normalized_weight)) {
        return options.max_normalized_weight;
    }
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(scaled)));
}

void normalize_weights(std::vector<Peer> &peers, const SmoothWeightedRoundRobin::Options &options) noexcept {
    std::int64_t divisor = 0;
    for (Peer &peer: peers) {
        peer.normalized_weight = quantize_weight(peer.endpoint.weight, options);
        divisor = std::gcd(divisor, peer.normalized_weight);
    }
    if (divisor > 1) {
        for (Peer &peer: peers) {
            peer.normalized_weight /= divisor;
        }
    }

    long double total = 0;
    for (const Peer &peer: peers) {
        total += peer.normalized_weight;
    }
    if (total <= static_cast<long double>(options.max_total_weight)) {
        return;
    }
    const long double scale = static_cast<long double>(options.max_total_weight) / total;
    for (Peer &peer: peers) {
        peer.normalized_weight = std::max<std::int64_t>(1, static_cast<std::int64_t>(peer.normalized_weight * scale));
    }
}

std::int64_t scaled_weight(std::int64_t value, std::int64_t old_base, std::int64_t new_base) noexcept {
    if (old_base <= 0) {
        return new_base;
    }
    const long double ratio =
            static_cast<long double>(value) * static_cast<long double>(new_base) / static_cast<long double>(old_base);
    return static_cast<std::int64_t>(std::llround(ratio));
}

struct ClusterMatch {
    bool matches = false;
    bool preferred = false;
};

ClusterMatch cluster_match(std::string_view raw_cluster, std::string_view cluster,
                           std::string_view preferred_zone) noexcept {
    if (cluster.empty()) {
        return {.matches = true, .preferred = true};
    }
    const std::size_t separator = raw_cluster.find('-');
    const std::string_view logical_cluster =
            separator == std::string_view::npos ? raw_cluster : raw_cluster.substr(separator + 1);
    if (logical_cluster != cluster) {
        return {};
    }
    if (preferred_zone.empty() || separator == std::string_view::npos) {
        return {.matches = true, .preferred = true};
    }
    return {.matches = true, .preferred = raw_cluster.substr(0, separator) == preferred_zone};
}

bool excluded(std::span<const std::uint64_t> peer_ids, std::uint64_t peer_id) noexcept {
    return std::find(peer_ids.begin(), peer_ids.end(), peer_id) != peer_ids.end();
}

bool circuit_available(const PeerRuntime &runtime, const SmoothWeightedRoundRobin::Options &options,
                       SmoothWeightedRoundRobin::TimePoint now) noexcept {
    return options.max_fails == 0 || runtime.fails < options.max_fails || now - runtime.checked > options.fail_timeout;
}

} // namespace

SmoothWeightedRoundRobin::Selection::Selection(std::shared_ptr<Generation> owner, std::size_t index,
                                               std::uint64_t peer_epoch) noexcept :
    owner_(std::move(owner)), index_(index), peer_epoch_(peer_epoch), pending_(true) {}

SmoothWeightedRoundRobin::Selection::Selection(Selection &&other) noexcept :
    owner_(std::move(other.owner_)), index_(other.index_), peer_epoch_(other.peer_epoch_), pending_(other.pending_) {
    other.pending_ = false;
}

SmoothWeightedRoundRobin::Selection &SmoothWeightedRoundRobin::Selection::operator=(Selection &&other) noexcept {
    if (this != &other) {
        owner_ = std::move(other.owner_);
        index_ = other.index_;
        peer_epoch_ = other.peer_epoch_;
        pending_ = other.pending_;
        other.pending_ = false;
    }
    return *this;
}

bool SmoothWeightedRoundRobin::Selection::valid() const noexcept {
    return owner_ && index_ < owner_->peers.size() && owner_->peers[index_].runtime->peer_epoch == peer_epoch_;
}

std::string_view SmoothWeightedRoundRobin::Selection::host() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].endpoint.host;
}

const std::optional<net::IpAddress> &SmoothWeightedRoundRobin::Selection::ip_address() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].endpoint.ip_address;
}

std::uint16_t SmoothWeightedRoundRobin::Selection::port() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].endpoint.port;
}

std::string_view SmoothWeightedRoundRobin::Selection::authority() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].endpoint.authority;
}

std::string_view SmoothWeightedRoundRobin::Selection::instance_id() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].endpoint.instance_id;
}

std::string_view SmoothWeightedRoundRobin::Selection::cluster_name() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].endpoint.cluster_name;
}

std::uint64_t SmoothWeightedRoundRobin::Selection::generation() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->generation;
}

std::uint64_t SmoothWeightedRoundRobin::Selection::peer_id() const noexcept {
    FIBER_ASSERT(valid());
    return peer_epoch_;
}

void SmoothWeightedRoundRobin::Selection::report(bool success) noexcept {
    report(success, event::EventLoop::current().now());
}

void SmoothWeightedRoundRobin::Selection::report(bool success, TimePoint now) noexcept {
    SmoothWeightedRoundRobin::complete(*this, success, now);
}

SmoothWeightedRoundRobin::SmoothWeightedRoundRobin() : SmoothWeightedRoundRobin(Options{}) {}

SmoothWeightedRoundRobin::SmoothWeightedRoundRobin(Options options) :
    core_(std::make_shared<Core>(std::move(options))) {
    FIBER_ASSERT(core_->options.max_fails <= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()));
    FIBER_ASSERT(core_->options.fail_timeout > std::chrono::milliseconds::zero());
    FIBER_ASSERT(core_->options.weight_precision > 0);
    FIBER_ASSERT(core_->options.max_normalized_weight > 0);
    FIBER_ASSERT(core_->options.max_total_weight >= core_->options.max_normalized_weight);
}

bool SmoothWeightedRoundRobin::update(const nacos::ServiceInfo &snapshot) {
    auto next = std::make_shared<Generation>();
    next->core = core_;
    next->checksum = snapshot.checksum;
    next->peers.reserve(snapshot.hosts.size());
    for (const nacos::ServiceInstance &instance: snapshot.hosts) {
        if (!instance.enabled || !instance.healthy || !std::isfinite(instance.weight) || instance.weight <= 0.0 ||
            instance.ip.empty() || instance.port == 0) {
            continue;
        }
        net::IpAddress ip;
        const bool parsed_ip = net::IpAddress::parse(instance.ip, ip);
        const std::optional<net::IpAddress> address = parsed_ip ? std::optional(ip) : std::nullopt;
        auto runtime = std::make_shared<PeerRuntime>();
        next->peers.push_back(Peer{
                .endpoint =
                        Endpoint{
                                .instance_id = std::string(instance.instance_id),
                                .host = std::string(instance.ip),
                                .ip_address = address,
                                .port = instance.port,
                                .authority = make_authority(instance.ip, instance.port, address),
                                .weight = instance.weight,
                                .cluster_name = std::string(instance.cluster_name),
                        },
                .runtime = std::move(runtime),
        });
    }
    std::sort(next->peers.begin(), next->peers.end(), [](const Peer &left, const Peer &right) {
        const int endpoint = compare_peer(left, right);
        if (endpoint != 0) {
            return endpoint < 0;
        }
        return left.endpoint.instance_id < right.endpoint.instance_id;
    });
    next->peers.erase(std::unique(next->peers.begin(), next->peers.end(),
                                  [](const Peer &left, const Peer &right) { return compare_peer(left, right) == 0; }),
                      next->peers.end());
    normalize_weights(next->peers, core_->options);

    std::lock_guard guard(core_->mutex);
    const std::shared_ptr<Generation> current = core_->current;
    if (current && same_generation(*current, *next)) {
        return false;
    }

    FIBER_ASSERT(!current || current->generation != std::numeric_limits<std::uint64_t>::max());
    next->generation = current ? current->generation + 1 : 1;
    std::size_t old_index = 0;
    std::size_t new_index = 0;
    while (current && old_index < current->peers.size() && new_index < next->peers.size()) {
        Peer &old_peer = current->peers[old_index];
        Peer &new_peer = next->peers[new_index];
        const int comparison = compare_peer(old_peer, new_peer);
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
        runtime->current_weight =
                scaled_weight(runtime->current_weight, runtime->base_weight, new_peer.normalized_weight);
        runtime->effective_weight =
                std::clamp(scaled_weight(runtime->effective_weight, runtime->base_weight, new_peer.normalized_weight),
                           std::int64_t{0}, new_peer.normalized_weight);
        runtime->base_weight = new_peer.normalized_weight;
        new_peer.runtime = runtime;
        ++old_index;
        ++new_index;
    }
    while (current && old_index < current->peers.size()) {
        current->peers[old_index++].runtime->retired = true;
    }
    for (Peer &peer: next->peers) {
        if (peer.runtime->peer_epoch == 0) {
            FIBER_ASSERT(core_->next_peer_epoch != std::numeric_limits<std::uint64_t>::max());
            peer.runtime->peer_epoch = ++core_->next_peer_epoch;
            peer.runtime->base_weight = peer.normalized_weight;
            peer.runtime->effective_weight = peer.normalized_weight;
        }
    }
    core_->current = std::move(next);
    return true;
}

std::expected<SmoothWeightedRoundRobin::Selection, ServiceSelectError>
SmoothWeightedRoundRobin::select(std::string_view cluster, std::string_view preferred_zone,
                                 std::span<const std::uint64_t> excluded_peer_ids) noexcept {
    return select(cluster, preferred_zone, excluded_peer_ids, event::EventLoop::current().now());
}

std::expected<SmoothWeightedRoundRobin::Selection, ServiceSelectError>
SmoothWeightedRoundRobin::select(std::string_view cluster, std::string_view preferred_zone,
                                 std::span<const std::uint64_t> excluded_peer_ids, TimePoint now) noexcept {
    std::lock_guard guard(core_->mutex);
    const std::shared_ptr<Generation> current = core_->current;
    if (!current || current->peers.empty()) {
        return std::unexpected(ServiceSelectError::NoAvailableInstance);
    }

    bool has_preferred = false;
    for (const Peer &peer: current->peers) {
        const ClusterMatch match = cluster_match(peer.endpoint.cluster_name, cluster, preferred_zone);
        if (match.matches && match.preferred && !peer.runtime->retired &&
            !excluded(excluded_peer_ids, peer.runtime->peer_epoch) &&
            circuit_available(*peer.runtime, core_->options, now)) {
            has_preferred = true;
            break;
        }
    }

    PeerRuntime *best = nullptr;
    std::size_t best_index = 0;
    std::int64_t total = 0;
    for (std::size_t i = 0; i < current->peers.size(); ++i) {
        Peer &peer = current->peers[i];
        const ClusterMatch match = cluster_match(peer.endpoint.cluster_name, cluster, preferred_zone);
        PeerRuntime &runtime = *peer.runtime;
        if (!match.matches || (has_preferred && !match.preferred) || runtime.retired ||
            excluded(excluded_peer_ids, runtime.peer_epoch) || !circuit_available(runtime, core_->options, now)) {
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
        return std::unexpected(ServiceSelectError::NoAvailableInstance);
    }

    best->current_weight -= total;
    if (now - best->checked > core_->options.fail_timeout) {
        best->checked = now;
    }
    return Selection(std::move(current), best_index, best->peer_epoch);
}

void SmoothWeightedRoundRobin::complete(Selection &selection, bool success, TimePoint now) noexcept {
    if (!selection.pending_ || !selection.valid()) {
        return;
    }
    const std::shared_ptr<Generation> generation = selection.owner_;
    const std::shared_ptr<Core> core = generation->core;
    std::lock_guard guard(core->mutex);
    PeerRuntime &runtime = *generation->peers[selection.index_].runtime;
    if (runtime.peer_epoch != selection.peer_epoch_) {
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

std::uint64_t SmoothWeightedRoundRobin::generation() const noexcept {
    std::lock_guard guard(core_->mutex);
    return core_->current ? core_->current->generation : 0;
}

std::size_t SmoothWeightedRoundRobin::configured_instance_count() const noexcept {
    std::lock_guard guard(core_->mutex);
    return core_->current ? core_->current->peers.size() : 0;
}

} // namespace fiber::access_server
