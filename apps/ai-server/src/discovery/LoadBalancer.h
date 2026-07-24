#ifndef FIBER_AI_SERVER_LOAD_BALANCER_H
#define FIBER_AI_SERVER_LOAD_BALANCER_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <http/Http1ConnectionGroupKey.h>
#include <net/SocketAddress.h>

namespace fiber::ai_server {

namespace detail {
struct RoundRobin;
}

struct DiscoveredInstance {
    std::string instance_id;
    net::SocketAddress address;
    http::Http1ConnectionGroupKey connection_key;
    std::string host_header;
    double weight = 1.0;
    std::string cluster_name;
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

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] const net::SocketAddress &address() const noexcept;
        [[nodiscard]] const http::Http1ConnectionGroupKey &connection_key() const noexcept;
        [[nodiscard]] std::string_view host_header() const noexcept;
        [[nodiscard]] std::string_view instance_id() const noexcept;
        [[nodiscard]] std::string_view cluster_name() const noexcept;
        [[nodiscard]] std::string_view service_name() const noexcept;
        [[nodiscard]] double configured_weight() const noexcept;
        [[nodiscard]] std::int64_t normalized_weight() const noexcept;
        [[nodiscard]] std::uint64_t generation() const noexcept;

    private:
        friend class LoadBalancer;

        Instance(std::shared_ptr<detail::RoundRobin> owner, std::size_t index, std::uint64_t peer_epoch) noexcept;
        void release_neutral() noexcept;

        // Pins the immutable discovery generation until the request reports or releases it.
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

    struct Core;
    static void complete_instance(Instance &instance, InstanceReportOutcome outcome, TimePoint now) noexcept;

    // Core is stable across generations, so all workers and updates share one SWRR/circuit lock domain.
    std::shared_ptr<Core> core_;
    std::atomic<std::shared_ptr<detail::RoundRobin>> current_;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_LOAD_BALANCER_H
