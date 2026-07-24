#include "../src/protocol/LlmError.h"
#include "../src/protocol/SseParser.h"
#include "../src/protocol/TokenUsage.h"
#include "../src/provider/ProviderErrorClassifier.h"

#include <chrono>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace fiber::ai_server {
namespace {

std::string_view view(const mem::IoBuf &buffer) {
    return {reinterpret_cast<const char *>(buffer.readable_data()), buffer.readable()};
}

TEST(LlmProtocolTest, EncodesProtocolSpecificErrors) {
    const LlmError error{
            .status_code = 403,
            .code = "model_not_available",
            .type = "invalid_request_error",
            .message = "model is not available",
            .field = "model",
    };
    auto openai = encode_llm_error(LlmWireProtocol::OpenAiChatCompletions, error);
    ASSERT_TRUE(openai.has_value());
    EXPECT_EQ(
            view(*openai),
            R"({"error":{"message":"model is not available","type":"invalid_request_error","param":"model","code":"model_not_available"}})");

    auto anthropic = encode_llm_error(LlmWireProtocol::AnthropicMessages, error);
    ASSERT_TRUE(anthropic.has_value());
    EXPECT_EQ(
            view(*anthropic),
            R"({"type":"error","error":{"type":"invalid_request_error","message":"model is not available"},"request_id":null})");
}

TEST(LlmProtocolTest, ExtractsAndMergesOpenAiUsage) {
    mem::BufPool pool;
    auto usage = extract_token_usage(
            LlmWireProtocol::OpenAiChatCompletions,
            R"({"usage":{"prompt_tokens":12,"completion_tokens":5,"total_tokens":17,"prompt_tokens_details":{"cached_tokens":3}}})",
            false, pool);
    ASSERT_TRUE(usage.has_value());
    EXPECT_EQ(usage->input_cached, 3);
    EXPECT_EQ(usage->input_uncached, 9);
    EXPECT_EQ(usage->output, 5);
    EXPECT_EQ(usage->total, 17);

    mem::BufPool chunk_pool;
    auto next = extract_token_usage(LlmWireProtocol::OpenAiChatCompletions,
                                    R"({"usage":{"completion_tokens":8,"total_tokens":20}})", true, chunk_pool);
    ASSERT_TRUE(next.has_value());
    usage->merge(*next);
    EXPECT_EQ(usage->output, 8);
    EXPECT_EQ(usage->total, 20);
}

TEST(LlmProtocolTest, ExtractsAnthropicResponseAndPartialEventUsage) {
    mem::BufPool response_pool;
    auto response = extract_token_usage(
            LlmWireProtocol::AnthropicMessages,
            R"({"usage":{"input_tokens":10,"output_tokens":4,"cache_creation_input_tokens":2,"cache_read_input_tokens":3}})",
            false, response_pool);
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->input_cached, 3);
    EXPECT_EQ(response->input_uncached, 12);
    EXPECT_EQ(response->output, 4);
    EXPECT_EQ(response->total, 19);

    mem::BufPool event_pool;
    auto event = extract_token_usage(LlmWireProtocol::AnthropicMessages,
                                     R"({"type":"message_delta","usage":{"output_tokens":7}})", true, event_pool);
    ASSERT_TRUE(event.has_value());
    EXPECT_FALSE(event->input_cached.has_value());
    EXPECT_FALSE(event->input_uncached.has_value());
    EXPECT_EQ(event->output, 7);
}

TEST(LlmProtocolTest, ParsesSseAcrossChunksAndNormalizesCrLf) {
    SseParser parser(LlmWireProtocol::AnthropicMessages);
    ASSERT_TRUE(parser.feed("event: message_start\r\ndata: {\"type\":", false));
    EXPECT_EQ(parser.next(), SseParseStatus::NeedMore);
    ASSERT_TRUE(parser.feed("\"message_start\"}\r\n\r\n", false));
    ASSERT_EQ(parser.next(), SseParseStatus::Event);
    EXPECT_EQ(parser.event().event_type, "message_start");
    EXPECT_EQ(parser.event().data, R"({"type":"message_start"})");
    EXPECT_EQ(parser.event().encoded, "event: message_start\ndata: {\"type\":\"message_start\"}\n\n");
    EXPECT_EQ(parser.next(), SseParseStatus::NeedMore);
    ASSERT_TRUE(parser.feed({}, true));
    EXPECT_EQ(parser.next(), SseParseStatus::Complete);
}

TEST(LlmProtocolTest, EmitsOnlyOneOpenAiDoneMarker) {
    SseParser parser(LlmWireProtocol::OpenAiChatCompletions);
    ASSERT_TRUE(parser.feed("data: {\"id\":1}\n\ndata: [DONE]\n\n"
                            "data: [DONE]\n\n",
                            true));
    ASSERT_EQ(parser.next(), SseParseStatus::Event);
    EXPECT_EQ(parser.event().data, R"({"id":1})");
    ASSERT_EQ(parser.next(), SseParseStatus::Event);
    EXPECT_TRUE(parser.event().terminal);
    EXPECT_EQ(parser.next(), SseParseStatus::Complete);
    EXPECT_TRUE(parser.done_seen());
}

TEST(LlmProtocolTest, ClassifiesTokenAndProviderFailures) {
    mem::BufPool pool;
    LoadBalanceConfig load_balance;
    auto token = classify_provider_response(LlmWireProtocol::OpenAiChatCompletions, 400, "7",
                                            R"({"error":{"type":"insufficient_quota"}})", load_balance, false, pool);
    EXPECT_EQ(token.scope, ProviderErrorScope::ApiToken);
    EXPECT_TRUE(token.retryable);
    EXPECT_EQ(token.unavailable_ttl, std::chrono::seconds(7));

    mem::BufPool status_pool;
    auto provider = classify_provider_response(LlmWireProtocol::AnthropicMessages, 503, {}, {}, load_balance, false,
                                               status_pool);
    EXPECT_EQ(provider.scope, ProviderErrorScope::Provider);
    EXPECT_TRUE(provider.retryable);

    auto started = classify_provider_transport_error(true);
    EXPECT_EQ(started.scope, ProviderErrorScope::Provider);
    EXPECT_FALSE(started.retryable);
}

} // namespace
} // namespace fiber::ai_server
