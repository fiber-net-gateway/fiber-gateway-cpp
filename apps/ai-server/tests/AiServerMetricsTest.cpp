#include <gtest/gtest.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <string_view>

#include <fiber/async/Spawn.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>

#include "observability/AiServerMetrics.h"

namespace {

using namespace std::chrono_literals;
using fiber::ai_server::AiServerMetrics;
using fiber::ai_server::LlmTokenUsage;
using fiber::ai_server::LlmWireProtocol;
using fiber::ai_server::ProviderHttpErrorCode;
using fiber::ai_server::RateLimitCheckMetric;
using fiber::ai_server::RateLimitSettleMetric;
using fiber::ai_server::SseDrainMetric;

std::string consume_chain(fiber::mem::IoBufChain chain) {
    std::string output;
    while (const fiber::mem::IoBuf *part = chain.first_readable()) {
        output.append(reinterpret_cast<const char *>(part->readable_data()), part->readable());
        chain.consume_and_compact(part->readable());
    }
    return output;
}

std::optional<std::string_view> metric_value(const std::string &metrics, std::string_view name) {
    std::string marker;
    marker.reserve(name.size() + 2);
    marker.push_back('\n');
    marker.append(name);
    marker.push_back(' ');
    const std::size_t line = metrics.find(marker);
    if (line == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t value = line + marker.size();
    const std::size_t end = metrics.find('\n', value);
    if (end == std::string::npos) {
        return std::nullopt;
    }
    return std::string_view(metrics).substr(value, end - value);
}

std::optional<std::uint64_t> parse_uint_value(std::string_view value) {
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<std::uint64_t> integer_metric_value(const std::string &metrics, std::string_view name) {
    auto value = metric_value(metrics, name);
    return value ? parse_uint_value(*value) : std::nullopt;
}

std::optional<std::uint64_t> seconds_metric_whole_value(const std::string &metrics, std::string_view name) {
    auto value = metric_value(metrics, name);
    if (!value) {
        return std::nullopt;
    }
    const std::size_t decimal = value->find('.');
    return parse_uint_value(value->substr(0, decimal));
}

fiber::async::DetachedTask record_worker_metrics(AiServerMetrics::Worker *worker, LlmWireProtocol protocol,
                                                 fiber::http::HttpResponseStats response,
                                                 std::promise<void> *done) noexcept {
    worker->request_started(protocol);
    worker->provider_attempt(protocol);
    worker->provider_failure(protocol);
    worker->provider_retry(protocol);
    worker->provider_transport_failure(protocol, protocol == LlmWireProtocol::OpenAiChatCompletions
                                                         ? ProviderHttpErrorCode::Dns
                                                         : ProviderHttpErrorCode::ReadHeader);
    worker->provider_attempts_skipped(protocol, protocol == LlmWireProtocol::OpenAiChatCompletions ? 2 : 3);
    worker->dns_backoff_hit(protocol);
    worker->provider_circuit_open(protocol);
    worker->rate_limit_check(RateLimitCheckMetric::Allowed);
    worker->rate_limit_settle(RateLimitSettleMetric::Usage);
    worker->sse_failure(protocol);
    worker->sse_drain(protocol, protocol == LlmWireProtocol::OpenAiChatCompletions ? SseDrainMetric::Completed
                                                                                   : SseDrainMetric::UpstreamError);
    worker->audit_generated();
    worker->audit_generation_failed();
    worker->audit_capture_incomplete();
    const LlmTokenUsage usage =
            protocol == LlmWireProtocol::OpenAiChatCompletions
                    ? LlmTokenUsage{
                              .in_cache = 2,
                              .in_nocache = 3,
                              .out = 5,
                              .total_tokens = 10,
                      }
                    : LlmTokenUsage{
                              .in_cache = 7,
                              .in_nocache = 11,
                              .out = 13,
                              .total_tokens = 31,
                      };
    worker->token_usage("alice", "primary", protocol, usage);
    worker->request_finished(protocol, response, 1500us);
    done->set_value();
    co_return;
}

fiber::async::DetachedTask collect_metrics(AiServerMetrics *metrics,
                                           std::promise<fiber::common::IoResult<std::string>> *done) noexcept {
    const fiber::log::AppenderStats audit_stats{
            .written_records = 13,
            .written_bytes = 4096,
            .dropped_records = 4,
            .write_errors = 2,
            .reopen_errors = 5,
            .rotations = 3,
            .rotation_errors = 1,
            .retention_errors = 6,
            .active_file_bytes = 2048,
    };
    auto collected = co_await metrics->collect(fiber::event::EventLoop::current().io_buf_node_pool(),
                                               fiber::ai_server::TokenRateLimiterStats{
                                                       .limiter_count = 3,
                                                       .in_flight_count = 1,
                                               },
                                               2, &audit_stats);
    if (!collected) {
        done->set_value(std::unexpected(collected.error()));
    } else {
        done->set_value(consume_chain(std::move(*collected)));
    }
    co_return;
}

fiber::async::DetachedTask stop_metrics(AiServerMetrics *metrics, std::promise<void> *done) noexcept {
    metrics->stop_collecting();
    co_await metrics->wait_for_idle();
    done->set_value();
}

TEST(AiServerMetricsTest, AggregatesRuntimeAndDynamicTokenUsageMetrics) {
    fiber::event::EventLoopGroup workers(2);
    AiServerMetrics metrics(workers);
    ASSERT_TRUE(metrics.valid());
    metrics.set_config_generation(7);

    std::promise<void> recorded0;
    std::promise<void> recorded1;
    auto recorded0_future = recorded0.get_future();
    auto recorded1_future = recorded1.get_future();
    workers.start();
    fiber::async::spawn(workers.at(0), [&]() {
        return record_worker_metrics(&metrics.worker(0), LlmWireProtocol::OpenAiChatCompletions,
                                     fiber::http::HttpResponseStats{
                                             .status_code = 200,
                                             .header_sent = true,
                                             .completed = true,
                                     },
                                     &recorded0);
    });
    fiber::async::spawn(workers.at(1), [&]() {
        return record_worker_metrics(&metrics.worker(1), LlmWireProtocol::AnthropicMessages,
                                     fiber::http::HttpResponseStats{
                                             .status_code = 401,
                                             .header_sent = true,
                                             .completed = true,
                                     },
                                     &recorded1);
    });
    ASSERT_EQ(recorded0_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(recorded1_future.wait_for(2s), std::future_status::ready);

    std::promise<fiber::common::IoResult<std::string>> collected;
    auto collected_future = collected.get_future();
    fiber::async::spawn(workers.at(0), [&]() { return collect_metrics(&metrics, &collected); });
    ASSERT_EQ(collected_future.wait_for(2s), std::future_status::ready);
    auto result = collected_future.get();
    ASSERT_TRUE(result);
    EXPECT_NE(result->find("ai_server_requests_total{protocol=\"openai\",result=\"success\"} 1"), std::string::npos);
    EXPECT_NE(result->find("ai_server_requests_total{protocol=\"anthropic\",result=\"client_error\"} 1"),
              std::string::npos);
    EXPECT_NE(result->find("ai_server_provider_attempts_total{protocol=\"openai\"} 1"), std::string::npos);
    EXPECT_NE(result->find("ai_server_provider_transport_failures_total{protocol=\"openai\",phase=\"dns\"} 1"),
              std::string::npos);
    EXPECT_NE(
            result->find("ai_server_provider_transport_failures_total{protocol=\"anthropic\",phase=\"read_header\"} 1"),
            std::string::npos);
    EXPECT_NE(result->find("ai_server_provider_attempts_skipped_total{protocol=\"openai\"} 2"), std::string::npos);
    EXPECT_NE(result->find("ai_server_provider_attempts_skipped_total{protocol=\"anthropic\"} 3"), std::string::npos);
    EXPECT_NE(result->find("ai_server_dns_backoff_hits_total{protocol=\"openai\"} 1"), std::string::npos);
    EXPECT_NE(result->find("ai_server_provider_circuit_opens_total{protocol=\"anthropic\"} 1"), std::string::npos);
    EXPECT_NE(result->find("ai_server_sse_drains_total{protocol=\"openai\",result=\"completed\"} 1"),
              std::string::npos);
    EXPECT_NE(result->find("ai_server_sse_drains_total{protocol=\"anthropic\",result=\"upstream_error\"} 1"),
              std::string::npos);
    EXPECT_NE(result->find("ai_server_rate_limit_checks_total{result=\"allowed\"} 2"), std::string::npos);
    EXPECT_NE(result->find("ai_server_user_token_usage_total{username=\"alice\",token_type=\"in_cache\"} 9"),
              std::string::npos);
    EXPECT_NE(result->find("ai_server_user_token_usage_total{username=\"alice\",token_type=\"in_nocache\"} 14"),
              std::string::npos);
    EXPECT_NE(result->find("ai_server_user_token_usage_total{username=\"alice\",token_type=\"out\"} 18"),
              std::string::npos);
    EXPECT_NE(result->find("ai_server_provider_token_usage_total{provider_name=\"primary\",protocol=\"openai\","
                           "token_type=\"in_cache\"} 2"),
              std::string::npos);
    EXPECT_NE(result->find("ai_server_provider_token_usage_total{provider_name=\"primary\",protocol=\"anthropic\","
                           "token_type=\"in_nocache\"} 11"),
              std::string::npos);
    EXPECT_NE(result->find("ai_server_provider_token_usage_total{provider_name=\"primary\",protocol=\"anthropic\","
                           "token_type=\"out\"} 13"),
              std::string::npos);
    EXPECT_NE(result->find("ai_server_config_generation 7"), std::string::npos);
    EXPECT_NE(result->find("ai_server_rate_limit_entries 3"), std::string::npos);
    EXPECT_NE(result->find("ai_server_rate_limit_inflight 1"), std::string::npos);
    EXPECT_NE(result->find("ai_server_rate_limit_cluster_nodes 2"), std::string::npos);
    EXPECT_NE(result->find("ai_server_audit_generated_records_total 2"), std::string::npos);
    EXPECT_NE(result->find("ai_server_audit_generation_failures_total 2"), std::string::npos);
    EXPECT_NE(result->find("ai_server_audit_capture_incomplete_total 2"), std::string::npos);
    EXPECT_NE(result->find("ai_server_audit_written_records_total 13"), std::string::npos);
    EXPECT_NE(result->find("ai_server_audit_written_bytes_total 4096"), std::string::npos);
    EXPECT_NE(result->find("ai_server_audit_dropped_records_total 4"), std::string::npos);
    EXPECT_NE(result->find("ai_server_audit_write_failures_total 2"), std::string::npos);
    EXPECT_NE(result->find("ai_server_audit_rotations_total 3"), std::string::npos);
    EXPECT_NE(result->find("ai_server_audit_rotation_failures_total 1"), std::string::npos);
    EXPECT_NE(result->find("ai_server_audit_reopen_failures_total 5"), std::string::npos);
    EXPECT_NE(result->find("ai_server_audit_retention_failures_total 6"), std::string::npos);
    EXPECT_NE(result->find("ai_server_audit_active_file_bytes 2048"), std::string::npos);

    EXPECT_NE(result->find("# TYPE process_cpu_seconds_total counter"), std::string::npos);
    const auto cpu_time = metric_value(*result, "process_cpu_seconds_total");
    ASSERT_TRUE(cpu_time);
    EXPECT_EQ(cpu_time->find_first_not_of("0123456789."), std::string_view::npos);
    const auto resident_memory = integer_metric_value(*result, "process_resident_memory_bytes");
    const auto virtual_memory = integer_metric_value(*result, "process_virtual_memory_bytes");
    ASSERT_TRUE(resident_memory);
    ASSERT_TRUE(virtual_memory);
    EXPECT_GT(*resident_memory, 0);
    EXPECT_GE(*virtual_memory, *resident_memory);
    const auto start_time = metric_value(*result, "process_start_time_seconds");
    ASSERT_TRUE(start_time);
    EXPECT_EQ(start_time->find_first_not_of("0123456789."), std::string_view::npos);
    EXPECT_GT(seconds_metric_whole_value(*result, "process_start_time_seconds").value_or(0), 1'000'000'000);
    const auto open_fds = integer_metric_value(*result, "process_open_fds");
    const auto max_fds = integer_metric_value(*result, "process_max_fds");
    ASSERT_TRUE(open_fds);
    ASSERT_TRUE(max_fds);
    EXPECT_GT(*open_fds, 0);
    EXPECT_GE(*max_fds, *open_fds);

    std::promise<void> stopped;
    auto stopped_future = stopped.get_future();
    fiber::async::spawn(workers.at(0), [&]() { return stop_metrics(&metrics, &stopped); });
    ASSERT_EQ(stopped_future.wait_for(2s), std::future_status::ready);
    workers.stop();
    workers.join();
}

} // namespace
