#include "../src/protocol/LlmError.h"
#include "../src/protocol/SseParser.h"
#include "../src/protocol/TokenUsage.h"
#include "../src/provider/ProviderErrorClassifier.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace fiber::ai_server {
namespace {

std::string_view view(const mem::IoBuf &buffer) {
    return {reinterpret_cast<const char *>(buffer.readable_data()), buffer.readable()};
}

mem::IoBuf make_buffer(std::string_view input) {
    mem::IoBuf buffer = mem::IoBuf::allocate(std::max<std::size_t>(input.size(), 1));
    if (!buffer) {
        return {};
    }
    if (!input.empty()) {
        std::memcpy(buffer.writable_data(), input.data(), input.size());
        buffer.commit(input.size());
    }
    return buffer;
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

TEST(LlmProtocolTest, ParsesSseAcrossChunksAndAssemblesSplitData) {
    SseParser parser;
    mem::IoBuf first = make_buffer("event: message_start\r\ndata: {\"type\":");
    ASSERT_TRUE(first);
    ASSERT_TRUE(parser.feed(first));
    EXPECT_EQ(parser.next(), SseParseStatus::NeedMore);

    mem::IoBuf second = make_buffer("\"message_start\"}\r\n\r\n");
    ASSERT_TRUE(second);
    ASSERT_TRUE(parser.feed(second));
    ASSERT_EQ(parser.next(), SseParseStatus::Event);
    EXPECT_EQ(parser.event().data, R"({"type":"message_start"})");
    EXPECT_EQ(parser.next(), SseParseStatus::NeedMore);
    ASSERT_TRUE(parser.finish());
    EXPECT_EQ(parser.next(), SseParseStatus::Complete);
}

TEST(LlmProtocolTest, BorrowsContiguousEventData) {
    constexpr std::string_view input = "event: delta\r\ndata: {\"id\":1}\r\n\r\n";
    mem::IoBuf buffer = make_buffer(input);
    ASSERT_TRUE(buffer);
    const std::string_view bytes = view(buffer);

    SseParser parser;
    ASSERT_TRUE(parser.feed(buffer));
    ASSERT_EQ(parser.next(), SseParseStatus::Event);
    EXPECT_EQ(parser.event().data, R"({"id":1})");
    EXPECT_EQ(parser.event().data.data(), bytes.data() + bytes.find(R"({"id":1})"));
}

TEST(LlmProtocolTest, RetainsContiguousDataWhenOnlyEventEndIsSplit) {
    mem::IoBuf data = make_buffer("data: retained\n");
    mem::IoBuf event_end = make_buffer("\n");
    ASSERT_TRUE(data);
    ASSERT_TRUE(event_end);
    const std::string_view input = view(data);
    const char *expected = input.data() + input.find("retained");

    SseParser parser;
    ASSERT_TRUE(parser.feed(data));
    EXPECT_EQ(parser.next(), SseParseStatus::NeedMore);
    ASSERT_TRUE(parser.feed(event_end));
    ASSERT_EQ(parser.next(), SseParseStatus::Event);
    EXPECT_EQ(parser.event().data, "retained");
    EXPECT_EQ(parser.event().data.data(), expected);
}

TEST(LlmProtocolTest, PreservesEveryDoneDataEvent) {
    mem::IoBuf buffer = make_buffer("data: {\"id\":1}\n\ndata: [DONE]\n\ndata: [DONE]\n\n");
    ASSERT_TRUE(buffer);
    SseParser parser;
    ASSERT_TRUE(parser.feed(buffer));
    ASSERT_EQ(parser.next(), SseParseStatus::Event);
    EXPECT_EQ(parser.event().data, R"({"id":1})");
    ASSERT_EQ(parser.next(), SseParseStatus::Event);
    EXPECT_EQ(parser.event().data, "[DONE]");
    ASSERT_EQ(parser.next(), SseParseStatus::Event);
    EXPECT_EQ(parser.event().data, "[DONE]");
    EXPECT_EQ(parser.next(), SseParseStatus::NeedMore);
    ASSERT_TRUE(parser.finish());
    EXPECT_EQ(parser.next(), SseParseStatus::Complete);
}

TEST(LlmProtocolTest, JoinsMultipleDataLinesAndIgnoresOtherFields) {
    mem::IoBuf buffer = make_buffer(": ping\r\nid: 7\r\nevent: delta\r\ndata: first\r\ndata: second\r\n\r\n");
    ASSERT_TRUE(buffer);
    SseParser parser;
    ASSERT_TRUE(parser.feed(buffer));
    ASSERT_EQ(parser.next(), SseParseStatus::Event);
    EXPECT_EQ(parser.event().data, "first\nsecond");
}

TEST(LlmProtocolTest, ParsesAnEventAcrossEveryByteSplit) {
    constexpr std::string_view input = "event: delta\r\ndata: {\"message\":\"hello\"}\r\n\r\n";
    for (std::size_t split = 1; split < input.size(); ++split) {
        SCOPED_TRACE(split);
        SseParser parser;
        mem::IoBuf first = make_buffer(input.substr(0, split));
        mem::IoBuf second = make_buffer(input.substr(split));
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        std::size_t event_count = 0;
        ASSERT_TRUE(parser.feed(first));
        SseParseStatus status = parser.next();
        while (status == SseParseStatus::Event) {
            EXPECT_EQ(parser.event().data, R"({"message":"hello"})");
            ++event_count;
            status = parser.next();
        }
        ASSERT_EQ(status, SseParseStatus::NeedMore);
        ASSERT_TRUE(parser.feed(second));
        status = parser.next();
        while (status == SseParseStatus::Event) {
            EXPECT_EQ(parser.event().data, R"({"message":"hello"})");
            ++event_count;
            status = parser.next();
        }
        EXPECT_EQ(event_count, 1u);
        EXPECT_EQ(status, SseParseStatus::NeedMore);
        ASSERT_TRUE(parser.finish());
        EXPECT_EQ(parser.next(), SseParseStatus::Complete);
    }
}

TEST(LlmProtocolTest, PreservesUtf8BytesAcrossChunks) {
    const std::string input = "data: 你好\n\n";
    const std::string_view bytes = input;
    mem::IoBuf first = make_buffer(bytes.substr(0, 8));
    mem::IoBuf second = make_buffer(bytes.substr(8));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    SseParser parser;
    ASSERT_TRUE(parser.feed(first));
    EXPECT_EQ(parser.next(), SseParseStatus::NeedMore);
    ASSERT_TRUE(parser.feed(second));
    ASSERT_EQ(parser.next(), SseParseStatus::Event);
    EXPECT_EQ(parser.event().data, "你好");
    EXPECT_EQ(parser.next(), SseParseStatus::NeedMore);
    ASSERT_TRUE(parser.finish());
    EXPECT_EQ(parser.next(), SseParseStatus::Complete);
}

TEST(LlmProtocolTest, TreatsMalformedUtf8AsOpaqueEventData) {
    std::string input = "data: ";
    input.push_back(static_cast<char>(0xc3));
    input.append("\n\n");
    mem::IoBuf buffer = make_buffer(input);
    ASSERT_TRUE(buffer);

    SseParser parser;
    ASSERT_TRUE(parser.feed(buffer));
    ASSERT_EQ(parser.next(), SseParseStatus::Event);
    ASSERT_EQ(parser.event().data.size(), 1u);
    EXPECT_EQ(static_cast<unsigned char>(parser.event().data.front()), 0xc3U);
    EXPECT_EQ(parser.next(), SseParseStatus::NeedMore);
    ASSERT_TRUE(parser.finish());
    EXPECT_EQ(parser.next(), SseParseStatus::Complete);
}

TEST(LlmProtocolTest, RejectsOversizedLogicalData) {
    mem::IoBuf buffer = make_buffer("data: 12345\n\n");
    ASSERT_TRUE(buffer);
    SseParser parser(4);
    ASSERT_TRUE(parser.feed(buffer));
    EXPECT_EQ(parser.next(), SseParseStatus::Error);
    EXPECT_EQ(parser.error(), SseParseError::DataTooLarge);
}

TEST(LlmProtocolTest, ClassifiesTokenAndProviderFailures) {
    mem::BufPool pool;
    LoadBalanceConfig load_balance;
    auto token = classify_provider_response(LlmWireProtocol::OpenAiChatCompletions, 400, "7",
                                            R"({"error":{"type":"insufficient_quota"}})", load_balance, false, pool);
    EXPECT_EQ(token.scope, ProviderErrorScope::ApiToken);
    EXPECT_EQ(token.instance_outcome, InstanceReportOutcome::Neutral);
    EXPECT_TRUE(token.retryable);
    EXPECT_EQ(token.unavailable_ttl, std::chrono::seconds(7));

    mem::BufPool status_pool;
    auto provider = classify_provider_response(LlmWireProtocol::AnthropicMessages, 503, {}, {}, load_balance, false,
                                               status_pool);
    EXPECT_EQ(provider.scope, ProviderErrorScope::Provider);
    EXPECT_EQ(provider.instance_outcome, InstanceReportOutcome::Failure);
    EXPECT_TRUE(provider.retryable);

    auto started = classify_provider_transport_error(true);
    EXPECT_EQ(started.scope, ProviderErrorScope::Provider);
    EXPECT_EQ(started.instance_outcome, InstanceReportOutcome::Failure);
    EXPECT_FALSE(started.retryable);
}

} // namespace
} // namespace fiber::ai_server
