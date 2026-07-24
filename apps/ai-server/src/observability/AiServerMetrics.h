#ifndef FIBER_AI_SERVER_METRICS_H
#define FIBER_AI_SERVER_METRICS_H

#include "../limit/TokenRateLimiter.h"
#include "../protocol/LlmBody.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <async/Task.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <common/mem/IoBufChain.h>
#include <event/EventLoopGroup.h>
#include <fiber/prometheus/Counter.h>
#include <fiber/prometheus/Gauge.h>
#include <fiber/prometheus/Histogram.h>
#include <fiber/prometheus/MetricsRegistry.h>
#include <http/HttpExchange.h>

namespace fiber::ai_server {

enum class RateLimitCheckMetric : std::uint8_t {
    Bypass,
    Allowed,
    Denied,
    Error,
    Count,
};

enum class RateLimitSettleMetric : std::uint8_t {
    Usage,
    NoUsage,
    Error,
    Count,
};

class AiServerMetrics final : public common::NonCopyable, public common::NonMovable {
public:
    class Worker {
    public:
        void request_started(LlmWireProtocol protocol) noexcept;
        void request_finished(LlmWireProtocol protocol, const http::HttpResponseStats &response,
                              std::chrono::microseconds duration) noexcept;
        void provider_attempt(LlmWireProtocol protocol) noexcept;
        void provider_failure(LlmWireProtocol protocol) noexcept;
        void provider_retry(LlmWireProtocol protocol) noexcept;
        void provider_circuit_open(LlmWireProtocol protocol) noexcept;
        void rate_limit_check(RateLimitCheckMetric result) noexcept;
        void rate_limit_settle(RateLimitSettleMetric result) noexcept;
        void sse_failure(LlmWireProtocol protocol) noexcept;

    private:
        friend class AiServerMetrics;

        static constexpr std::size_t kProtocolCount = 2;
        static constexpr std::size_t kRequestResultCount = 4;

        std::array<std::array<prometheus::CounterRef, kRequestResultCount>, kProtocolCount> requests_;
        std::array<prometheus::HistogramRef, kProtocolCount> request_duration_;
        std::array<prometheus::GaugeRef, kProtocolCount> inflight_;
        std::array<prometheus::CounterRef, kProtocolCount> provider_attempts_;
        std::array<prometheus::CounterRef, kProtocolCount> provider_failures_;
        std::array<prometheus::CounterRef, kProtocolCount> provider_retries_;
        std::array<prometheus::CounterRef, kProtocolCount> provider_circuit_opens_;
        std::array<prometheus::CounterRef, static_cast<std::size_t>(RateLimitCheckMetric::Count)> rate_limit_checks_;
        std::array<prometheus::CounterRef, static_cast<std::size_t>(RateLimitSettleMetric::Count)> rate_limit_settles_;
        std::array<prometheus::CounterRef, kProtocolCount> sse_failures_;
    };

    explicit AiServerMetrics(event::EventLoopGroup &workers);
    ~AiServerMetrics();

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] Worker &worker(std::size_t index) noexcept;

    void set_config_generation(std::uint64_t generation) noexcept;
    [[nodiscard]] async::Task<common::IoResult<mem::IoBufChain>>
    collect(mem::IoBufNodePool &node_pool, TokenRateLimiterStats limiter_stats, std::size_t cluster_nodes) noexcept;

    void stop_collecting() noexcept;
    [[nodiscard]] async::Task<void> wait_for_idle() noexcept;

private:
    [[nodiscard]] bool initialize(event::EventLoopGroup &workers);

    prometheus::MetricsRegistry registry_;
    std::vector<Worker> workers_;
    std::atomic<std::uint64_t> config_generation_{0};
    bool collecting_stopped_ = false;
    bool valid_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_METRICS_H
