#ifndef FIBER_ACCESS_SERVER_SMOOTH_WEIGHTED_ROUND_ROBIN_H
#define FIBER_ACCESS_SERVER_SMOOTH_WEIGHTED_ROUND_ROBIN_H

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::access_server {

enum class SwrrSelectError : std::uint8_t {
    NoConfiguredInstance,
    NoAvailableInstance,
};

// SmoothWeightedRoundRobin deliberately knows nothing about the value it
// selects. The caller owns routing/grouping semantics and supplies an opaque
// instance plus a stable, non-zero selection token and its weight.
template<std::equality_comparable Instance>
class SmoothWeightedRoundRobin final : public common::NonCopyable, public common::NonMovable {
private:
    struct Weighted;
    struct WeightedStorage;
    struct State;

public:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct WeightedInstance {
        std::uint64_t selection_token = 0;
        Instance instance;
        double weight = 1.0;
    };

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
        [[nodiscard]] const Instance &instance() const noexcept;
        [[nodiscard]] std::uint64_t generation() const noexcept;
        [[nodiscard]] std::uint64_t selection_token() const noexcept;
        void report(bool success) noexcept;
        void report(bool success, TimePoint now) noexcept;

    private:
        friend class SmoothWeightedRoundRobin;

        Selection(std::weak_ptr<State> state, std::shared_ptr<Weighted> weighted_instances,
                  std::size_t weighted_instance_count, std::size_t index, std::uint64_t generation,
                  std::uint64_t selection_token) noexcept;

        std::weak_ptr<State> state_;
        std::shared_ptr<Weighted> weighted_instances_;
        std::size_t weighted_instance_count_ = 0;
        std::size_t index_ = 0;
        std::uint64_t generation_ = 0;
        std::uint64_t selection_token_ = 0;
        bool pending_ = false;
    };

    SmoothWeightedRoundRobin();
    explicit SmoothWeightedRoundRobin(Options options);
    ~SmoothWeightedRoundRobin() = default;

    // Control-plane update. selection_token must be non-zero and unique in
    // the caller's routing domain. Reusing a token preserves that instance's
    // SWRR and failure state across updates.
    [[nodiscard]] bool update(std::vector<WeightedInstance> instances);
    [[nodiscard]] std::expected<Selection, SwrrSelectError>
    select(std::span<const std::uint64_t> excluded_selection_tokens = {}) noexcept;
    [[nodiscard]] std::expected<Selection, SwrrSelectError>
    select(std::span<const std::uint64_t> excluded_selection_tokens, TimePoint now) noexcept;

    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::size_t configured_instance_count() const noexcept;

private:
    static void complete(Selection &selection, bool success, TimePoint now) noexcept;

    std::shared_ptr<State> state_;
};

} // namespace fiber::access_server

#include "SmoothWeightedRoundRobinImpl.h"

#endif // FIBER_ACCESS_SERVER_SMOOTH_WEIGHTED_ROUND_ROBIN_H
