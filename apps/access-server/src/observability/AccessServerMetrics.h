#ifndef FIBER_ACCESS_SERVER_ACCESS_SERVER_METRICS_H
#define FIBER_ACCESS_SERVER_ACCESS_SERVER_METRICS_H

#include <array>
#include <chrono>
#include <cstddef>
#include <vector>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/prometheus/Counter.h>
#include <fiber/prometheus/Gauge.h>
#include <fiber/prometheus/Histogram.h>
#include <fiber/prometheus/MetricsRegistry.h>

namespace fiber::access_server {

class AccessServerMetrics final : public common::NonCopyable, public common::NonMovable {
public:
    class Worker {
    public:
        void request_started() noexcept;
        void request_finished(const http::HttpResponseStats &response, std::chrono::microseconds duration) noexcept;

    private:
        friend class AccessServerMetrics;

        static constexpr std::size_t kRequestResultCount = 4;

        std::array<prometheus::CounterRef, kRequestResultCount> requests_;
        prometheus::HistogramRef request_duration_;
        prometheus::GaugeRef inflight_;
    };

    explicit AccessServerMetrics(event::EventLoopGroup &workers);
    ~AccessServerMetrics();

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] Worker &worker(std::size_t index) noexcept;

    [[nodiscard]] async::Task<common::IoResult<mem::IoBufChain>> collect(mem::IoBufNodePool &node_pool) noexcept;

    void stop_collecting() noexcept;
    [[nodiscard]] async::Task<void> wait_for_idle() noexcept;

private:
    [[nodiscard]] bool initialize(event::EventLoopGroup &workers);

    prometheus::MetricsRegistry registry_;
    std::vector<Worker> workers_;
    bool collecting_stopped_ = false;
    bool valid_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SERVER_METRICS_H
