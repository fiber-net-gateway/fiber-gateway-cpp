#include "AiServerMetrics.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>

#include <common/Assert.h>
#include <common/mem/IoBuf.h>

namespace fiber::ai_server {
namespace {

constexpr std::size_t protocol_index(LlmWireProtocol protocol) noexcept {
    return protocol == LlmWireProtocol::OpenAiChatCompletions ? 0 : 1;
}

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

void append_gauge(std::string &output, std::string_view name, std::string_view help, std::uint64_t value) {
    output.append("# HELP ");
    output.append(name);
    output.push_back(' ');
    output.append(help);
    output.append("\n# TYPE ");
    output.append(name);
    output.append(" gauge\n");
    output.append(name);
    output.push_back(' ');
    output.append(std::to_string(value));
    output.push_back('\n');
}

} // namespace

void AiServerMetrics::Worker::request_started(LlmWireProtocol protocol) noexcept {
    inflight_[protocol_index(protocol)].inc();
}

void AiServerMetrics::Worker::request_finished(LlmWireProtocol protocol, const http::HttpResponseStats &response,
                                               std::chrono::microseconds duration) noexcept {
    const std::size_t index = protocol_index(protocol);
    inflight_[index].dec();
    requests_[index][request_result_index(response)].inc();
    request_duration_[index].observe(static_cast<std::uint64_t>(std::max<std::int64_t>(duration.count(), 0)));
}

void AiServerMetrics::Worker::provider_attempt(LlmWireProtocol protocol) noexcept {
    provider_attempts_[protocol_index(protocol)].inc();
}

void AiServerMetrics::Worker::provider_failure(LlmWireProtocol protocol) noexcept {
    provider_failures_[protocol_index(protocol)].inc();
}

void AiServerMetrics::Worker::provider_retry(LlmWireProtocol protocol) noexcept {
    provider_retries_[protocol_index(protocol)].inc();
}

void AiServerMetrics::Worker::provider_circuit_open(LlmWireProtocol protocol) noexcept {
    provider_circuit_opens_[protocol_index(protocol)].inc();
}

void AiServerMetrics::Worker::rate_limit_check(RateLimitCheckMetric result) noexcept {
    rate_limit_checks_[static_cast<std::size_t>(result)].inc();
}

void AiServerMetrics::Worker::rate_limit_settle(RateLimitSettleMetric result) noexcept {
    rate_limit_settles_[static_cast<std::size_t>(result)].inc();
}

void AiServerMetrics::Worker::sse_failure(LlmWireProtocol protocol) noexcept {
    sse_failures_[protocol_index(protocol)].inc();
}

AiServerMetrics::AiServerMetrics(event::EventLoopGroup &workers) { valid_ = initialize(workers); }

AiServerMetrics::~AiServerMetrics() { FIBER_ASSERT(!valid_ || collecting_stopped_); }

bool AiServerMetrics::initialize(event::EventLoopGroup &worker_group) {
    constexpr std::array<std::string_view, 1> kProtocolLabel{"protocol"};
    constexpr std::array<std::string_view, 2> kRequestLabels{"protocol", "result"};
    constexpr std::array<std::string_view, 1> kResultLabel{"result"};
    constexpr std::array<std::string_view, 2> kProtocols{"openai", "anthropic"};
    constexpr std::array<std::string_view, 4> kRequestResults{"success", "client_error", "server_error", "canceled"};
    constexpr std::array<std::string_view, 4> kCheckResults{"bypass", "allowed", "denied", "error"};
    constexpr std::array<std::string_view, 3> kSettleResults{"usage", "no_usage", "error"};
    constexpr std::array<std::uint64_t, 15> kDurationBounds{
            1000,    5000,    10000,   25000,    50000,    100000,   250000,    500000,
            1000000, 2500000, 5000000, 10000000, 30000000, 60000000, 300000000,
    };

    auto requests = registry_.register_counter("ai_server_requests_total", "Completed LLM requests.", kRequestLabels);
    auto duration =
            registry_.register_histogram("ai_server_request_duration_seconds", "LLM request duration.", kDurationBounds,
                                         prometheus::HistogramUnit::Microseconds, kProtocolLabel);
    auto inflight = registry_.register_gauge("ai_server_requests_inflight", "In-flight LLM requests.",
                                             prometheus::GaugeReduction::Sum, kProtocolLabel);
    auto provider_attempts =
            registry_.register_counter("ai_server_provider_attempts_total", "Provider attempts.", kProtocolLabel);
    auto provider_failures = registry_.register_counter("ai_server_provider_failures_total",
                                                        "Failed Provider attempts.", kProtocolLabel);
    auto provider_retries = registry_.register_counter("ai_server_provider_retries_total", "Retried Provider attempts.",
                                                       kProtocolLabel);
    auto provider_circuit_opens = registry_.register_counter("ai_server_provider_circuit_opens_total",
                                                             "Provider circuit breaker openings.", kProtocolLabel);
    auto rate_checks = registry_.register_counter("ai_server_rate_limit_checks_total",
                                                  "Token rate limit check outcomes.", kResultLabel);
    auto rate_settles = registry_.register_counter("ai_server_rate_limit_settlements_total",
                                                   "Token rate limit settlements.", kResultLabel);
    auto sse_failures = registry_.register_counter("ai_server_sse_failures_total", "SSE failures after response start.",
                                                   kProtocolLabel);
    if (!requests || !duration || !inflight || !provider_attempts || !provider_failures || !provider_retries ||
        !provider_circuit_opens || !rate_checks || !rate_settles || !sse_failures) {
        return false;
    }

    std::array<std::array<prometheus::SeriesId, 4>, 2> request_series;
    std::array<prometheus::SeriesId, 2> duration_series;
    std::array<prometheus::SeriesId, 2> inflight_series;
    std::array<prometheus::SeriesId, 2> attempt_series;
    std::array<prometheus::SeriesId, 2> failure_series;
    std::array<prometheus::SeriesId, 2> retry_series;
    std::array<prometheus::SeriesId, 2> circuit_open_series;
    std::array<prometheus::SeriesId, 2> sse_series;
    std::array<prometheus::SeriesId, 4> check_series;
    std::array<prometheus::SeriesId, 3> settle_series;
    for (std::size_t protocol = 0; protocol < kProtocols.size(); ++protocol) {
        for (std::size_t result = 0; result < kRequestResults.size(); ++result) {
            auto series = registry_.register_series(*requests, std::array<std::string_view, 2>{
                                                                       kProtocols[protocol],
                                                                       kRequestResults[result],
                                                               });
            if (!series) {
                return false;
            }
            request_series[protocol][result] = *series;
        }
        auto duration_id = registry_.register_series(*duration, std::array<std::string_view, 1>{kProtocols[protocol]});
        auto inflight_id = registry_.register_series(*inflight, std::array<std::string_view, 1>{kProtocols[protocol]});
        auto attempt_id =
                registry_.register_series(*provider_attempts, std::array<std::string_view, 1>{kProtocols[protocol]});
        auto failure_id =
                registry_.register_series(*provider_failures, std::array<std::string_view, 1>{kProtocols[protocol]});
        auto retry_id =
                registry_.register_series(*provider_retries, std::array<std::string_view, 1>{kProtocols[protocol]});
        auto circuit_open_id = registry_.register_series(*provider_circuit_opens,
                                                         std::array<std::string_view, 1>{kProtocols[protocol]});
        auto sse_id = registry_.register_series(*sse_failures, std::array<std::string_view, 1>{kProtocols[protocol]});
        if (!duration_id || !inflight_id || !attempt_id || !failure_id || !retry_id || !circuit_open_id || !sse_id) {
            return false;
        }
        duration_series[protocol] = *duration_id;
        inflight_series[protocol] = *inflight_id;
        attempt_series[protocol] = *attempt_id;
        failure_series[protocol] = *failure_id;
        retry_series[protocol] = *retry_id;
        circuit_open_series[protocol] = *circuit_open_id;
        sse_series[protocol] = *sse_id;
    }
    for (std::size_t i = 0; i < kCheckResults.size(); ++i) {
        auto series = registry_.register_series(*rate_checks, std::array<std::string_view, 1>{kCheckResults[i]});
        if (!series) {
            return false;
        }
        check_series[i] = *series;
    }
    for (std::size_t i = 0; i < kSettleResults.size(); ++i) {
        auto series = registry_.register_series(*rate_settles, std::array<std::string_view, 1>{kSettleResults[i]});
        if (!series) {
            return false;
        }
        settle_series[i] = *series;
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
        for (std::size_t protocol = 0; protocol < kProtocols.size(); ++protocol) {
            for (std::size_t result = 0; result < kRequestResults.size(); ++result) {
                auto value = shard->counter(request_series[protocol][result]);
                if (!value) {
                    return false;
                }
                worker.requests_[protocol][result] = *value;
            }
            auto duration_value = shard->histogram(duration_series[protocol]);
            auto inflight_value = shard->gauge(inflight_series[protocol]);
            auto attempt_value = shard->counter(attempt_series[protocol]);
            auto failure_value = shard->counter(failure_series[protocol]);
            auto retry_value = shard->counter(retry_series[protocol]);
            auto circuit_open_value = shard->counter(circuit_open_series[protocol]);
            auto sse_value = shard->counter(sse_series[protocol]);
            if (!duration_value || !inflight_value || !attempt_value || !failure_value || !retry_value ||
                !circuit_open_value || !sse_value) {
                return false;
            }
            worker.request_duration_[protocol] = *duration_value;
            worker.inflight_[protocol] = *inflight_value;
            worker.provider_attempts_[protocol] = *attempt_value;
            worker.provider_failures_[protocol] = *failure_value;
            worker.provider_retries_[protocol] = *retry_value;
            worker.provider_circuit_opens_[protocol] = *circuit_open_value;
            worker.sse_failures_[protocol] = *sse_value;
        }
        for (std::size_t i = 0; i < check_series.size(); ++i) {
            auto value = shard->counter(check_series[i]);
            if (!value) {
                return false;
            }
            worker.rate_limit_checks_[i] = *value;
        }
        for (std::size_t i = 0; i < settle_series.size(); ++i) {
            auto value = shard->counter(settle_series[i]);
            if (!value) {
                return false;
            }
            worker.rate_limit_settles_[i] = *value;
        }
    }
    return true;
}

AiServerMetrics::Worker &AiServerMetrics::worker(std::size_t index) noexcept {
    FIBER_ASSERT(valid_);
    FIBER_ASSERT(index < workers_.size());
    return workers_[index];
}

void AiServerMetrics::set_config_generation(std::uint64_t generation) noexcept {
    std::uint64_t current = config_generation_.load(std::memory_order_relaxed);
    while (current < generation && !config_generation_.compare_exchange_weak(
                                           current, generation, std::memory_order_release, std::memory_order_relaxed)) {
    }
}

async::Task<common::IoResult<mem::IoBufChain>> AiServerMetrics::collect(mem::IoBufNodePool &node_pool,
                                                                        TokenRateLimiterStats limiter_stats,
                                                                        std::size_t cluster_nodes) noexcept {
    auto collected = co_await registry_.collect_text(node_pool);
    if (!collected) {
        co_return std::unexpected(collected.error());
    }
    std::string gauges;
    gauges.reserve(512);
    append_gauge(gauges, "ai_server_config_generation", "Latest installed configuration generation.",
                 config_generation_.load(std::memory_order_acquire));
    append_gauge(gauges, "ai_server_rate_limit_entries", "Local token rate limiter entries.",
                 limiter_stats.limiter_count);
    append_gauge(gauges, "ai_server_rate_limit_inflight", "Local token rate limiter in-flight sessions.",
                 limiter_stats.in_flight_count);
    append_gauge(gauges, "ai_server_rate_limit_cluster_nodes", "Token rate limit shard ring nodes.", cluster_nodes);
    mem::IoBuf tail = mem::IoBuf::allocate(gauges.size());
    if (!tail) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    std::memcpy(tail.writable_data(), gauges.data(), gauges.size());
    tail.commit(gauges.size());
    if (!collected->append(std::move(tail))) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    co_return std::move(*collected);
}

void AiServerMetrics::stop_collecting() noexcept {
    if (!valid_ || collecting_stopped_) {
        return;
    }
    registry_.stop_collecting();
    collecting_stopped_ = true;
}

async::Task<void> AiServerMetrics::wait_for_idle() noexcept {
    if (valid_) {
        co_await registry_.wait_for_idle();
    }
}

} // namespace fiber::ai_server
