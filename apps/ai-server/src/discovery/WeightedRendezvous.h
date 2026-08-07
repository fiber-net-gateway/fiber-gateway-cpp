#ifndef FIBER_AI_SERVER_WEIGHTED_RENDEZVOUS_H
#define FIBER_AI_SERVER_WEIGHTED_RENDEZVOUS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/net/IpAddress.h>

namespace fiber::nacos {
struct ServiceInfo;
}

namespace fiber::ai_server {

enum class ServiceSelectError : std::uint8_t {
    NoAvailableInstance,
};

enum class InstanceReportOutcome : std::uint8_t {
    Success,
    Failure,
    Neutral,
};

namespace detail {
struct RendezvousGeneration;
}

class WeightedRendezvous final : public common::NonCopyable, public common::NonMovable {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Options {
        std::size_t max_fails = 3;
        std::chrono::milliseconds fail_timeout = std::chrono::seconds(30);
        std::int64_t weight_precision = 1000;
        std::int64_t max_normalized_weight = 1'000'000;
        std::int64_t max_total_weight = 1'000'000'000;
    };

    class Selection final {
    public:
        Selection() noexcept = default;
        Selection(const Selection &) = delete;
        Selection &operator=(const Selection &) = delete;
        Selection(Selection &&other) noexcept;
        Selection &operator=(Selection &&other) noexcept;
        ~Selection() { report(InstanceReportOutcome::Neutral, TimePoint{}); }

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] bool pending() const noexcept { return pending_; }
        [[nodiscard]] std::string_view host() const noexcept;
        [[nodiscard]] const net::IpAddress &ip_address() const noexcept;
        [[nodiscard]] std::uint16_t port() const noexcept;
        [[nodiscard]] std::string_view authority() const noexcept;
        [[nodiscard]] std::string_view instance_id() const noexcept;
        [[nodiscard]] std::uint64_t generation() const noexcept;
        [[nodiscard]] std::uint64_t peer_id() const noexcept;
        void report(InstanceReportOutcome outcome) noexcept;
        void report(InstanceReportOutcome outcome, TimePoint now) noexcept;

    private:
        friend class WeightedRendezvous;

        Selection(std::shared_ptr<detail::RendezvousGeneration> owner, std::size_t index,
                  std::uint64_t peer_epoch) noexcept;

        std::shared_ptr<detail::RendezvousGeneration> owner_;
        std::size_t index_ = 0;
        std::uint64_t peer_epoch_ = 0;
        bool pending_ = false;
    };

    WeightedRendezvous();
    explicit WeightedRendezvous(Options options);
    ~WeightedRendezvous() = default;

    [[nodiscard]] bool update(const nacos::ServiceInfo &snapshot);
    [[nodiscard]] std::expected<Selection, ServiceSelectError>
    select(std::uint64_t key, std::span<const std::uint64_t> excluded_peer_ids = {}) noexcept;
    [[nodiscard]] std::expected<Selection, ServiceSelectError>
    select(std::uint64_t key, std::span<const std::uint64_t> excluded_peer_ids, TimePoint now) noexcept;

    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::size_t configured_instance_count() const noexcept;

private:
    friend struct detail::RendezvousGeneration;

    struct Core;
    static void complete(Selection &selection, InstanceReportOutcome outcome, TimePoint now) noexcept;

    std::shared_ptr<Core> core_;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_WEIGHTED_RENDEZVOUS_H
