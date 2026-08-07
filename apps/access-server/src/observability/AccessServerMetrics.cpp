#include "AccessServerMetrics.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

#include <fiber/common/Assert.h>

namespace fiber::access_server {
namespace {

std::size_t request_result_index(const http::HttpResponseStats &response) noexcept {
    if (response.terminal_error != common::IoErr::None || !response.completed) {
        return 3;
    }
    if (response.status_code >= 200 && response.status_code < 400) {
        return 0;
    }
    if (response.status_code >= 400 && response.status_code < 500) {
        return 1;
    }
    return 2;
}

} // namespace

void AccessServerMetrics::Worker::request_started() noexcept { inflight_.inc(); }

void AccessServerMetrics::Worker::request_finished(const http::HttpResponseStats &response,
                                                   std::chrono::microseconds duration) noexcept {
    inflight_.dec();
    requests_[request_result_index(response)].inc();
    request_duration_.observe(static_cast<std::uint64_t>(std::max<std::int64_t>(duration.count(), 0)));
}

AccessServerMetrics::AccessServerMetrics(event::EventLoopGroup &workers) { valid_ = initialize(workers); }

AccessServerMetrics::~AccessServerMetrics() { FIBER_ASSERT(!valid_ || collecting_stopped_); }

bool AccessServerMetrics::initialize(event::EventLoopGroup &worker_group) {
    constexpr std::array<std::string_view, 1> kResultLabel{"result"};
    constexpr std::array<std::string_view, 4> kRequestResults{
            "success",
            "client_error",
            "server_error",
            "canceled",
    };
    constexpr std::array<std::uint64_t, 15> kDurationBounds{
            1000,    5000,    10000,   25000,    50000,    100000,   250000,    500000,
            1000000, 2500000, 5000000, 10000000, 30000000, 60000000, 300000000,
    };

    auto requests = registry_.register_counter("access_server_requests_total", "Completed access-server requests.",
                                               kResultLabel);
    auto duration =
            registry_.register_histogram("access_server_request_duration_seconds", "Access-server request duration.",
                                         kDurationBounds, prometheus::HistogramUnit::Microseconds);
    auto inflight = registry_.register_gauge("access_server_requests_inflight", "In-flight access-server requests.",
                                             prometheus::GaugeReduction::Sum);
    if (!requests || !duration || !inflight) {
        return false;
    }

    std::array<prometheus::SeriesId, kRequestResults.size()> request_series;
    for (std::size_t i = 0; i < kRequestResults.size(); ++i) {
        auto series = registry_.register_series(*requests, std::array<std::string_view, 1>{kRequestResults[i]});
        if (!series) {
            return false;
        }
        request_series[i] = *series;
    }
    auto duration_series = registry_.register_series(*duration);
    auto inflight_series = registry_.register_series(*inflight);
    if (!duration_series || !inflight_series) {
        return false;
    }

    std::vector<prometheus::ShardId> shard_ids;
    shard_ids.reserve(worker_group.size());
    for (std::size_t i = 0; i < worker_group.size(); ++i) {
        auto shard = registry_.add_shard(worker_group.at(i));
        if (!shard) {
            return false;
        }
        shard_ids.push_back(*shard);
    }
    if (!registry_.freeze()) {
        return false;
    }

    workers_.resize(worker_group.size());
    for (std::size_t worker_index = 0; worker_index < workers_.size(); ++worker_index) {
        prometheus::MetricsShard *shard = registry_.shard(shard_ids[worker_index]);
        if (!shard) {
            return false;
        }
        Worker &worker = workers_[worker_index];
        for (std::size_t result = 0; result < request_series.size(); ++result) {
            auto value = shard->counter(request_series[result]);
            if (!value) {
                return false;
            }
            worker.requests_[result] = *value;
        }
        auto duration_value = shard->histogram(*duration_series);
        auto inflight_value = shard->gauge(*inflight_series);
        if (!duration_value || !inflight_value) {
            return false;
        }
        worker.request_duration_ = *duration_value;
        worker.inflight_ = *inflight_value;
    }
    return true;
}

AccessServerMetrics::Worker &AccessServerMetrics::worker(std::size_t index) noexcept {
    FIBER_ASSERT(valid_);
    FIBER_ASSERT(index < workers_.size());
    return workers_[index];
}

async::Task<common::IoResult<mem::IoBufChain>> AccessServerMetrics::collect(mem::IoBufNodePool &node_pool) noexcept {
    co_return co_await registry_.collect_text(node_pool);
}

void AccessServerMetrics::stop_collecting() noexcept {
    if (!valid_ || collecting_stopped_) {
        return;
    }
    registry_.stop_collecting();
    collecting_stopped_ = true;
}

async::Task<void> AccessServerMetrics::wait_for_idle() noexcept {
    if (valid_) {
        co_await registry_.wait_for_idle();
    }
}

} // namespace fiber::access_server
