#ifndef FIBER_NACOS_DISCOVERY_SERVICE_LOAD_BALANCER_H
#define FIBER_NACOS_DISCOVERY_SERVICE_LOAD_BALANCER_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <net/IpAddress.h>

#include "ServiceDiscoveryTypes.h"

namespace fiber::nacos {

struct ServiceInfo;

namespace detail {
struct RoundRobin;
}

struct DiscoveredInstance {
    std::string instance_id;
    std::string host;
    std::optional<net::IpAddress> ip_address;
    std::uint16_t port = 0;
    std::string authority;
    double weight = 1.0;
    std::string cluster_name;
    std::uint64_t selection_hash = 0;
};

struct DiscoveredService {
    std::string service_name;
    std::string group;
    std::string checksum;
    std::int64_t last_ref_time = 0;
    std::vector<DiscoveredInstance> instances;
};

enum class LoadBalanceError : std::uint8_t {
    Uninitialized,
    NoAvailableInstance,
    Shutdown,
};

enum class InstanceReportOutcome : std::uint8_t {
    Success,
    Failure,
    Neutral,
};

enum class ServiceInstancePolicy : std::uint8_t {
    SmoothWeightedRoundRobin,
    WeightedRendezvous,
};

struct ServiceInstanceSelection {
    ServiceInstancePolicy policy = ServiceInstancePolicy::SmoothWeightedRoundRobin;
    // Empty selects across every Nacos cluster. Otherwise a raw cluster name
    // "zone-cluster" is matched by its cluster suffix and preferred_zone
    // selects the local tier first. A name without '-' is treated as local.
    std::string_view cluster;
    std::string_view preferred_zone;
    std::uint64_t rendezvous_key = 0;
    std::span<const std::uint64_t> excluded_peer_ids;
};

enum class LoadBalancerUpdateResult : std::uint8_t {
    Applied,
    Unchanged,
};

struct LoadBalancerStats {
    std::uint64_t generation = 0;
    std::size_t configured_instances = 0;
    std::uint64_t selections = 0;
    std::uint64_t unavailable = 0;
    std::uint64_t success_reports = 0;
    std::uint64_t failure_reports = 0;
    std::uint64_t neutral_reports = 0;
    std::uint64_t circuit_opens = 0;
};

class LoadBalancer final : public common::NonCopyable, public common::NonMovable {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Options {
        std::size_t max_fails = 3;
        std::chrono::milliseconds fail_timeout = std::chrono::seconds(30);
        bool fail_open_when_single = false;
        std::int64_t weight_precision = 1000;
        std::int64_t max_normalized_weight = 1'000'000;
        std::int64_t max_total_weight = 1'000'000'000;
    };

    class Instance final {
    public:
        Instance() noexcept = default;
        ~Instance();

        Instance(const Instance &) = delete;
        Instance &operator=(const Instance &) = delete;
        Instance(Instance &&other) noexcept;
        Instance &operator=(Instance &&other) noexcept;

        // The selected generation stays pinned, including after report(), so
        // all returned views remain valid until this Instance is destroyed.
        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] bool pending() const noexcept { return pending_; }
        [[nodiscard]] std::string_view host() const noexcept;
        [[nodiscard]] const std::optional<net::IpAddress> &ip_address() const noexcept;
        [[nodiscard]] std::uint16_t port() const noexcept;
        [[nodiscard]] std::string_view authority() const noexcept;
        [[nodiscard]] std::string_view instance_id() const noexcept;
        [[nodiscard]] std::string_view cluster_name() const noexcept;
        [[nodiscard]] std::string_view service_name() const noexcept;
        [[nodiscard]] double configured_weight() const noexcept;
        [[nodiscard]] std::int64_t normalized_weight() const noexcept;
        [[nodiscard]] std::uint64_t generation() const noexcept;
        [[nodiscard]] std::uint64_t peer_id() const noexcept;
        void report(InstanceReportOutcome outcome) noexcept;
        void report(InstanceReportOutcome outcome, TimePoint now) noexcept;

    private:
        friend class LoadBalancer;
        friend class LoadBalancerOps;

        Instance(std::shared_ptr<detail::RoundRobin> owner, std::size_t index, std::uint64_t peer_epoch) noexcept;
        void release_neutral() noexcept;

        // A pending instance released without an explicit report is neutral.
        std::shared_ptr<detail::RoundRobin> owner_;
        std::size_t index_ = 0;
        std::uint64_t peer_epoch_ = 0;
        bool pending_ = false;
    };

    LoadBalancer();
    explicit LoadBalancer(Options options);
    ~LoadBalancer();

    [[nodiscard]] std::expected<Instance, LoadBalanceError> load_balance() noexcept;
    [[nodiscard]] std::expected<Instance, LoadBalanceError> load_balance(TimePoint now) noexcept;
    [[nodiscard]] std::expected<Instance, LoadBalanceError>
    load_balance(const ServiceInstanceSelection &selection) noexcept;
    [[nodiscard]] std::expected<Instance, LoadBalanceError> load_balance(const ServiceInstanceSelection &selection,
                                                                         TimePoint now) noexcept;
    [[nodiscard]] std::expected<Instance, LoadBalanceError>
    load_balance(std::uint64_t key, std::span<const std::uint64_t> excluded_peer_ids = {}) noexcept;
    [[nodiscard]] std::expected<Instance, LoadBalanceError>
    load_balance(std::uint64_t key, std::span<const std::uint64_t> excluded_peer_ids, TimePoint now) noexcept;

    void report(Instance &instance, bool success) noexcept;
    void report(Instance &instance, bool success, TimePoint now) noexcept;
    void report(Instance &&instance, InstanceReportOutcome outcome) noexcept;
    void report(Instance &&instance, InstanceReportOutcome outcome, TimePoint now) noexcept;

    // Control-plane operation. Production callers should route updates through ServiceDiscovery.
    [[nodiscard]] LoadBalancerUpdateResult update_instances(DiscoveredService update);

    void shutdown() noexcept;

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::size_t configured_instance_count() const noexcept;
    [[nodiscard]] LoadBalancerStats stats() const noexcept;

private:
    friend struct detail::RoundRobin;
    friend class LoadBalancerOps;

    struct Core;
    static void complete_instance(Instance &instance, InstanceReportOutcome outcome, TimePoint now) noexcept;
    [[nodiscard]] std::shared_ptr<detail::RoundRobin> load_current() const noexcept;
    void store_current(std::shared_ptr<detail::RoundRobin> current) noexcept;

    // Core is stable across generations, so all workers and updates share one selection/circuit lock domain.
    std::shared_ptr<Core> core_;
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<detail::RoundRobin>> current_;
#else
    std::shared_ptr<detail::RoundRobin> current_;
#endif
};

// State operations used by BasicServiceDiscovery. The first Nacos snapshot
// constructs an initialized LoadBalancer; later snapshots update that stable
// state while request workers retain it independently through shared_ptr.
class LoadBalancerOps {
public:
    using State = LoadBalancer;
    using StatePtr = std::shared_ptr<State>;
    using UpdateResult = LoadBalancerUpdateResult;
    using CreateResult = ServiceStateCreateResult<StatePtr, UpdateResult>;

    struct Options {
        LoadBalancer::Options load_balancer;
        bool require_ip = false;
    };

    LoadBalancerOps() noexcept = default;
    LoadBalancerOps(Options options) noexcept : options_(std::move(options)) {}

    [[nodiscard]] CreateResult create(std::string_view service_name, std::string_view group,
                                      const std::shared_ptr<const ServiceInfo> &snapshot);
    [[nodiscard]] UpdateResult update(State &state, std::string_view service_name, std::string_view group,
                                      const std::shared_ptr<const ServiceInfo> &snapshot);
    void retire(State &) noexcept {}

    [[nodiscard]] static std::expected<State::Instance, LoadBalanceError>
    select(State &state, const ServiceInstanceSelection &selection, State::TimePoint now) noexcept;

private:
    [[nodiscard]] DiscoveredService make_update(std::string_view service_name, std::string_view group,
                                                const ServiceInfo &info) const;

    Options options_;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_DISCOVERY_SERVICE_LOAD_BALANCER_H
