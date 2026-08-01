#ifndef FIBER_ACCESS_SERVER_SMOOTH_WEIGHTED_ROUND_ROBIN_H
#define FIBER_ACCESS_SERVER_SMOOTH_WEIGHTED_ROUND_ROBIN_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <net/IpAddress.h>

namespace fiber::nacos {
struct ServiceInfo;
}

namespace fiber::access_server {

enum class ServiceSelectError : std::uint8_t {
    NoAvailableInstance,
};

namespace detail {
struct SwrrGeneration;
}

class SmoothWeightedRoundRobin final : public common::NonCopyable, public common::NonMovable {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Options {
        std::size_t max_fails = 25;
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
        ~Selection() = default;

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] bool pending() const noexcept { return pending_; }
        [[nodiscard]] std::string_view host() const noexcept;
        [[nodiscard]] const std::optional<net::IpAddress> &ip_address() const noexcept;
        [[nodiscard]] std::uint16_t port() const noexcept;
        [[nodiscard]] std::string_view authority() const noexcept;
        [[nodiscard]] std::string_view instance_id() const noexcept;
        [[nodiscard]] std::string_view cluster_name() const noexcept;
        [[nodiscard]] std::uint64_t generation() const noexcept;
        [[nodiscard]] std::uint64_t peer_id() const noexcept;
        void report(bool success) noexcept;
        void report(bool success, TimePoint now) noexcept;

    private:
        friend class SmoothWeightedRoundRobin;

        Selection(std::shared_ptr<detail::SwrrGeneration> owner, std::size_t index, std::uint64_t peer_epoch) noexcept;

        std::shared_ptr<detail::SwrrGeneration> owner_;
        std::size_t index_ = 0;
        std::uint64_t peer_epoch_ = 0;
        bool pending_ = false;
    };

    SmoothWeightedRoundRobin();
    explicit SmoothWeightedRoundRobin(Options options);
    ~SmoothWeightedRoundRobin() = default;

    [[nodiscard]] bool update(const nacos::ServiceInfo &snapshot);
    [[nodiscard]] std::expected<Selection, ServiceSelectError>
    select(std::string_view cluster, std::string_view preferred_zone,
           std::span<const std::uint64_t> excluded_peer_ids = {}) noexcept;
    [[nodiscard]] std::expected<Selection, ServiceSelectError> select(std::string_view cluster,
                                                                      std::string_view preferred_zone,
                                                                      std::span<const std::uint64_t> excluded_peer_ids,
                                                                      TimePoint now) noexcept;

    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::size_t configured_instance_count() const noexcept;

private:
    friend struct detail::SwrrGeneration;

    struct Core;

    static void complete(Selection &selection, bool success, TimePoint now) noexcept;

    std::shared_ptr<Core> core_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_SMOOTH_WEIGHTED_ROUND_ROBIN_H
