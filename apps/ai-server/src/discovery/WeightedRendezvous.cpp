#include "WeightedRendezvous.h"

#include <algorithm>
#include <array>
#include <bit>
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

namespace fiber::ai_server {

struct WeightedRendezvous::Core {
    explicit Core(Options value_options) noexcept : options(std::move(value_options)) {}

    Options options;
    std::mutex mutex;
    std::shared_ptr<detail::RendezvousGeneration> current;
    std::uint64_t next_peer_epoch = 0;
};

namespace detail {

struct RendezvousEndpoint {
    std::string instance_id;
    std::string host;
    net::IpAddress ip_address;
    std::uint16_t port = 0;
    std::string authority;
    double weight = 1.0;
    std::uint64_t selection_hash = 0;
};

struct RendezvousPeerRuntime {
    std::size_t fails = 0;
    WeightedRendezvous::TimePoint accessed{};
    WeightedRendezvous::TimePoint checked{};
    std::uint64_t peer_epoch = 0;
    bool retired = false;
};

struct RendezvousPeer {
    RendezvousEndpoint endpoint;
    std::int64_t normalized_weight = 1;
    std::shared_ptr<RendezvousPeerRuntime> runtime;
};

struct RendezvousGeneration {
    std::shared_ptr<WeightedRendezvous::Core> core;
    std::uint64_t generation = 0;
    std::string checksum;
    std::vector<RendezvousPeer> peers;
};

} // namespace detail

namespace {

using Endpoint = detail::RendezvousEndpoint;
using Generation = detail::RendezvousGeneration;
using Peer = detail::RendezvousPeer;
using PeerRuntime = detail::RendezvousPeerRuntime;

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void hash_byte(std::uint64_t &hash, std::uint8_t byte) noexcept {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= kFnvPrime;
}

void hash_be16(std::uint64_t &hash, std::uint16_t value) noexcept {
    hash_byte(hash, static_cast<std::uint8_t>(value >> 8U));
    hash_byte(hash, static_cast<std::uint8_t>(value & 0xffU));
}

void hash_be32(std::uint64_t &hash, std::uint32_t value) noexcept {
    hash_byte(hash, static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    hash_byte(hash, static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    hash_byte(hash, static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    hash_byte(hash, static_cast<std::uint8_t>(value & 0xffU));
}

std::uint64_t endpoint_hash(const Endpoint &endpoint) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    hash_byte(hash, 1U);
    hash_byte(hash, 0U);
    hash_be16(hash, endpoint.port);
    hash_byte(hash, static_cast<std::uint8_t>(endpoint.ip_address.family()));
    if (endpoint.ip_address.is_v4()) {
        for (std::uint8_t byte: endpoint.ip_address.v4_bytes()) {
            hash_byte(hash, byte);
        }
        return hash;
    }
    for (std::uint8_t byte: endpoint.ip_address.v6_bytes()) {
        hash_byte(hash, byte);
    }
    hash_be32(hash, endpoint.ip_address.scope_id());
    return hash;
}

std::uint64_t mix_hash(std::uint64_t key, std::uint64_t peer_hash) noexcept {
    std::uint64_t value = key ^ (peer_hash + 0x9e3779b97f4a7c15ULL);
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value;
}

double negative_log_uniform(std::uint64_t hash) noexcept {
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

double rendezvous_cost(std::uint64_t hash, std::int64_t weight) noexcept {
    return negative_log_uniform(hash) / static_cast<double>(weight);
}

std::string make_authority(const net::IpAddress &ip, std::string_view host, std::uint16_t port) {
    std::array<char, 5> port_text{};
    const auto converted = std::to_chars(port_text.data(), port_text.data() + port_text.size(), port);
    std::string result;
    result.reserve(host.size() + 8);
    if (ip.is_v6()) {
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
    const int address = compare_ip(left.endpoint.ip_address, right.endpoint.ip_address);
    if (address != 0) {
        return address;
    }
    if (left.endpoint.port != right.endpoint.port) {
        return left.endpoint.port < right.endpoint.port ? -1 : 1;
    }
    return 0;
}

bool same_definition(const Peer &left, const Peer &right) noexcept {
    return compare_peer(left, right) == 0 && left.endpoint.instance_id == right.endpoint.instance_id &&
           left.endpoint.host == right.endpoint.host && left.endpoint.authority == right.endpoint.authority &&
           left.endpoint.weight == right.endpoint.weight && left.normalized_weight == right.normalized_weight;
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

std::int64_t quantize_weight(double weight, const WeightedRendezvous::Options &options) noexcept {
    const long double scaled = static_cast<long double>(weight) * options.weight_precision;
    if (scaled >= static_cast<long double>(options.max_normalized_weight)) {
        return options.max_normalized_weight;
    }
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(scaled)));
}

void normalize_weights(std::vector<Peer> &peers, const WeightedRendezvous::Options &options) noexcept {
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

bool excluded(std::span<const std::uint64_t> peer_ids, std::uint64_t peer_id) noexcept {
    return std::find(peer_ids.begin(), peer_ids.end(), peer_id) != peer_ids.end();
}

bool circuit_available(const PeerRuntime &runtime, const WeightedRendezvous::Options &options,
                       WeightedRendezvous::TimePoint now) noexcept {
    return options.max_fails == 0 || runtime.fails < options.max_fails || now - runtime.checked > options.fail_timeout;
}

} // namespace

WeightedRendezvous::Selection::Selection(std::shared_ptr<Generation> owner, std::size_t index,
                                         std::uint64_t peer_epoch) noexcept :
    owner_(std::move(owner)), index_(index), peer_epoch_(peer_epoch), pending_(true) {}

WeightedRendezvous::Selection::Selection(Selection &&other) noexcept :
    owner_(std::move(other.owner_)), index_(other.index_), peer_epoch_(other.peer_epoch_), pending_(other.pending_) {
    other.pending_ = false;
}

WeightedRendezvous::Selection &WeightedRendezvous::Selection::operator=(Selection &&other) noexcept {
    if (this != &other) {
        report(InstanceReportOutcome::Neutral, TimePoint{});
        owner_ = std::move(other.owner_);
        index_ = other.index_;
        peer_epoch_ = other.peer_epoch_;
        pending_ = other.pending_;
        other.pending_ = false;
    }
    return *this;
}

bool WeightedRendezvous::Selection::valid() const noexcept {
    return owner_ && index_ < owner_->peers.size() && owner_->peers[index_].runtime->peer_epoch == peer_epoch_;
}

std::string_view WeightedRendezvous::Selection::host() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].endpoint.host;
}

const net::IpAddress &WeightedRendezvous::Selection::ip_address() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].endpoint.ip_address;
}

std::uint16_t WeightedRendezvous::Selection::port() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].endpoint.port;
}

std::string_view WeightedRendezvous::Selection::authority() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].endpoint.authority;
}

std::string_view WeightedRendezvous::Selection::instance_id() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->peers[index_].endpoint.instance_id;
}

std::uint64_t WeightedRendezvous::Selection::generation() const noexcept {
    FIBER_ASSERT(valid());
    return owner_->generation;
}

std::uint64_t WeightedRendezvous::Selection::peer_id() const noexcept {
    FIBER_ASSERT(valid());
    return peer_epoch_;
}

void WeightedRendezvous::Selection::report(InstanceReportOutcome outcome) noexcept {
    report(outcome, event::EventLoop::current().now());
}

void WeightedRendezvous::Selection::report(InstanceReportOutcome outcome, TimePoint now) noexcept {
    WeightedRendezvous::complete(*this, outcome, now);
}

WeightedRendezvous::WeightedRendezvous() : WeightedRendezvous(Options{}) {}

WeightedRendezvous::WeightedRendezvous(Options options) : core_(std::make_shared<Core>(std::move(options))) {
    FIBER_ASSERT(core_->options.fail_timeout > std::chrono::milliseconds::zero());
    FIBER_ASSERT(core_->options.weight_precision > 0);
    FIBER_ASSERT(core_->options.max_normalized_weight > 0);
    FIBER_ASSERT(core_->options.max_total_weight >= core_->options.max_normalized_weight);
}

bool WeightedRendezvous::update(const nacos::ServiceInfo &snapshot) {
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
        if (!net::IpAddress::parse(instance.ip, ip)) {
            continue;
        }
        Endpoint endpoint{
                .instance_id = std::string(instance.instance_id),
                .host = std::string(instance.ip),
                .ip_address = ip,
                .port = instance.port,
                .authority = make_authority(ip, instance.ip, instance.port),
                .weight = instance.weight,
        };
        endpoint.selection_hash = endpoint_hash(endpoint);
        next->peers.push_back(Peer{
                .endpoint = std::move(endpoint),
                .runtime = std::make_shared<PeerRuntime>(),
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
        new_peer.runtime = old_peer.runtime;
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
        }
    }
    core_->current = std::move(next);
    return true;
}

std::expected<WeightedRendezvous::Selection, ServiceSelectError>
WeightedRendezvous::select(std::uint64_t key, std::span<const std::uint64_t> excluded_peer_ids) noexcept {
    return select(key, excluded_peer_ids, event::EventLoop::current().now());
}

std::expected<WeightedRendezvous::Selection, ServiceSelectError>
WeightedRendezvous::select(std::uint64_t key, std::span<const std::uint64_t> excluded_peer_ids,
                           TimePoint now) noexcept {
    std::lock_guard guard(core_->mutex);
    const std::shared_ptr<Generation> current = core_->current;
    if (!current || current->peers.empty()) {
        return std::unexpected(ServiceSelectError::NoAvailableInstance);
    }

    const Peer *best = nullptr;
    std::size_t best_index = 0;
    double best_cost = 0;
    for (std::size_t i = 0; i < current->peers.size(); ++i) {
        const Peer &peer = current->peers[i];
        if (peer.runtime->retired || excluded(excluded_peer_ids, peer.runtime->peer_epoch) ||
            !circuit_available(*peer.runtime, core_->options, now)) {
            continue;
        }
        const double cost = rendezvous_cost(mix_hash(key, peer.endpoint.selection_hash), peer.normalized_weight);
        if (!best || cost < best_cost || (cost == best_cost && compare_peer(peer, *best) < 0)) {
            best = &peer;
            best_index = i;
            best_cost = cost;
        }
    }
    if (!best) {
        return std::unexpected(ServiceSelectError::NoAvailableInstance);
    }
    if (now - best->runtime->checked > core_->options.fail_timeout) {
        best->runtime->checked = now;
    }
    return Selection(std::move(current), best_index, best->runtime->peer_epoch);
}

void WeightedRendezvous::complete(Selection &selection, InstanceReportOutcome outcome, TimePoint now) noexcept {
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

    if (outcome == InstanceReportOutcome::Success) {
        if (!runtime.retired && runtime.accessed < runtime.checked) {
            runtime.fails = 0;
        }
    } else if (outcome == InstanceReportOutcome::Failure && !runtime.retired) {
        const TimePoint failure_time = std::max(now, runtime.checked);
        if (runtime.fails != std::numeric_limits<std::size_t>::max()) {
            ++runtime.fails;
        }
        runtime.accessed = failure_time;
        runtime.checked = failure_time;
    }
    selection.pending_ = false;
}

std::uint64_t WeightedRendezvous::generation() const noexcept {
    std::lock_guard guard(core_->mutex);
    return core_->current ? core_->current->generation : 0;
}

std::size_t WeightedRendezvous::configured_instance_count() const noexcept {
    std::lock_guard guard(core_->mutex);
    return core_->current ? core_->current->peers.size() : 0;
}

} // namespace fiber::ai_server
