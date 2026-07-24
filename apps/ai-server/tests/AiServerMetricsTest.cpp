#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <string>

#include <async/Spawn.h>
#include <common/IoError.h>
#include <event/EventLoop.h>
#include <event/EventLoopGroup.h>

#include "observability/AiServerMetrics.h"

namespace {

using namespace std::chrono_literals;
using fiber::ai_server::AiServerMetrics;
using fiber::ai_server::LlmWireProtocol;
using fiber::ai_server::RateLimitCheckMetric;
using fiber::ai_server::RateLimitSettleMetric;

std::string consume_chain(fiber::mem::IoBufChain chain) {
    std::string output;
    while (const fiber::mem::IoBuf *part = chain.first_readable()) {
        output.append(reinterpret_cast<const char *>(part->readable_data()), part->readable());
        chain.consume_and_compact(part->readable());
    }
    return output;
}

fiber::async::DetachedTask record_worker_metrics(AiServerMetrics::Worker *worker, LlmWireProtocol protocol,
                                                 fiber::http::HttpResponseStats response,
                                                 std::promise<void> *done) noexcept {
    worker->request_started(protocol);
    worker->provider_attempt(protocol);
    worker->provider_failure(protocol);
    worker->provider_retry(protocol);
    worker->provider_circuit_open(protocol);
    worker->rate_limit_check(RateLimitCheckMetric::Allowed);
    worker->rate_limit_settle(RateLimitSettleMetric::Usage);
    worker->sse_failure(protocol);
    worker->request_finished(protocol, response, 1500us);
    done->set_value();
    co_return;
}

fiber::async::DetachedTask collect_metrics(AiServerMetrics *metrics,
                                           std::promise<fiber::common::IoResult<std::string>> *done) noexcept {
    auto collected = co_await metrics->collect(fiber::event::EventLoop::current().io_buf_node_pool(),
                                               fiber::ai_server::TokenRateLimiterStats{
                                                       .limiter_count = 3,
                                                       .in_flight_count = 1,
                                               },
                                               2);
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

TEST(AiServerMetricsTest, AggregatesFixedSchemaAndRuntimeGauges) {
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
    EXPECT_NE(result->find("ai_server_provider_circuit_opens_total{protocol=\"anthropic\"} 1"), std::string::npos);
    EXPECT_NE(result->find("ai_server_rate_limit_checks_total{result=\"allowed\"} 2"), std::string::npos);
    EXPECT_NE(result->find("ai_server_config_generation 7"), std::string::npos);
    EXPECT_NE(result->find("ai_server_rate_limit_entries 3"), std::string::npos);
    EXPECT_NE(result->find("ai_server_rate_limit_inflight 1"), std::string::npos);
    EXPECT_NE(result->find("ai_server_rate_limit_cluster_nodes 2"), std::string::npos);

    std::promise<void> stopped;
    auto stopped_future = stopped.get_future();
    fiber::async::spawn(workers.at(0), [&]() { return stop_metrics(&metrics, &stopped); });
    ASSERT_EQ(stopped_future.wait_for(2s), std::future_status::ready);
    workers.stop();
    workers.join();
}

} // namespace
