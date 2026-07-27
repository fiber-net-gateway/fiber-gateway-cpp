#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "protocol/LlmBody.h"

namespace {

using fiber::ai_server::LlmBodyErrorCode;
using fiber::ai_server::LlmWireProtocol;
using fiber::ai_server::ParsedLlmBody;
using fiber::mem::BufPool;
using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;
using fiber::mem::IoBufNodePool;

IoBuf make_body(std::string_view text) {
    IoBuf body = IoBuf::allocate(text.size());
    if (!text.empty()) {
        std::memcpy(body.writable_data(), text.data(), text.size());
        body.commit(text.size());
    }
    return body;
}

std::string flatten(const IoBufChain &chain) {
    std::vector<iovec> parts(chain.size());
    const int count = chain.fill_write_iov(parts.data(), static_cast<int>(parts.size()));
    std::string result;
    result.reserve(chain.readable_bytes());
    for (int i = 0; i < count; ++i) {
        result.append(static_cast<const char *>(parts[i].iov_base), parts[i].iov_len);
    }
    return result;
}

TEST(LlmBodyTest, ExtractsOpenAiRoutingFieldsAndRetainsRawBody) {
    constexpr std::string_view input = R"({
  "model": "chat.public",
  "stream": true,
  "metadata": {"routeKey": "camel", "route_key": "preferred"},
  "prompt_cache_key": "cache-17",
  "messages": [
    {"role": "system", "content": "be concise"},
    {"role": "user", "content": 42},
    {"role": "assistant", "content": [{"type": "text", "text": "hello"}]}
  ]
})";
    BufPool pool;

    auto body = ParsedLlmBody::parse(LlmWireProtocol::OpenAiChatCompletions, make_body(input), pool);

    ASSERT_TRUE(body) << body.error().message;
    const auto &routing = body->routing();
    ASSERT_TRUE(routing.model.is_present());
    EXPECT_EQ(*routing.model, "chat.public");
    ASSERT_TRUE(routing.stream.is_present());
    EXPECT_TRUE(*routing.stream);
    ASSERT_TRUE(routing.metadata_route_key.is_present());
    EXPECT_EQ(*routing.metadata_route_key, "preferred");
    ASSERT_TRUE(routing.prompt_cache_key.is_present());
    EXPECT_EQ(*routing.prompt_cache_key, "cache-17");
    ASSERT_EQ(routing.message_roles.size(), 3u);
    EXPECT_EQ(*routing.message_roles[0], "system");
    EXPECT_EQ(*routing.message_roles[1], "user");
    EXPECT_EQ(*routing.message_roles[2], "assistant");
    ASSERT_EQ(routing.message_content_texts.size(), 3u);
    EXPECT_EQ(*routing.message_content_texts[0], "be concise");
    EXPECT_EQ(*routing.message_content_texts[1], "42");
    EXPECT_TRUE(routing.message_content_texts[2].is_null());
    EXPECT_EQ(routing.messages_count, 3u);
    EXPECT_EQ(body->body_size(), input.size());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(body->raw_body().readable_data()),
                               body->raw_body().readable()),
              input);
}

TEST(LlmBodyTest, ExtractsAnthropicSpecificFieldsAndNullsComplexValues) {
    constexpr std::string_view input = R"({
        "model":"claude.public",
        "container":"session-a",
        "system":[{"type":"text","text":"rules"}],
        "messages":[
            {"role":"user","content":"hello"},
            {"role":"assistant","content":{"type":"text","text":"world"}}
        ]
    })";
    BufPool pool;

    auto body = ParsedLlmBody::parse(LlmWireProtocol::AnthropicMessages, make_body(input), pool);

    ASSERT_TRUE(body) << body.error().message;
    const auto &routing = body->routing();
    ASSERT_TRUE(routing.container.is_present());
    EXPECT_EQ(*routing.container, "session-a");
    EXPECT_TRUE(routing.prompt_cache_key.is_absent());
    EXPECT_TRUE(routing.system_text.is_null());
    ASSERT_EQ(routing.message_content_texts.size(), 2u);
    EXPECT_EQ(*routing.message_content_texts[0], "hello");
    EXPECT_TRUE(routing.message_content_texts[1].is_null());
}

TEST(LlmBodyTest, CountsMessagesAndToolsWithoutBuildingDuplicatePromptParts) {
    constexpr std::string_view input = R"({
        "model":"claude.public",
        "system":[{"type":"text","text":"system rules"}],
        "messages":[{
            "role":"user",
            "content":[
                {"type":"text","text":"visible prompt"},
                {"type":"image","source":{"type":"base64","data":"SECRET_BASE64"}},
                {"type":"document","source":{"type":"url","url":"https://example.test/a?signature=SECRET"}}
            ]
        }],
        "tools":[{"name":"weather","description":"look up weather","input_schema":{"type":"object"}}]
    })";
    BufPool pool;

    auto body = ParsedLlmBody::parse(LlmWireProtocol::AnthropicMessages, make_body(input), pool);

    ASSERT_TRUE(body) << body.error().message;
    const auto &routing = body->routing();
    EXPECT_EQ(routing.messages_count, 1u);
    EXPECT_EQ(routing.tools_count, 1u);
    EXPECT_EQ(body->body_size(), input.size());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(body->raw_body().readable_data()),
                               body->raw_body().readable()),
              input);
}

TEST(LlmBodyTest, RejectsInvalidBodyAndRoutingFieldTypes) {
    BufPool pool;

    auto malformed = ParsedLlmBody::parse(LlmWireProtocol::OpenAiChatCompletions, make_body(R"({"model":"x")"), pool);
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code, LlmBodyErrorCode::InvalidJson);

    auto array_root = ParsedLlmBody::parse(LlmWireProtocol::OpenAiChatCompletions, make_body(R"(["x"])"), pool);
    ASSERT_FALSE(array_root);
    EXPECT_EQ(array_root.error().code, LlmBodyErrorCode::ExpectedObject);

    auto model_number =
            ParsedLlmBody::parse(LlmWireProtocol::OpenAiChatCompletions, make_body(R"({"model":17})"), pool);
    ASSERT_FALSE(model_number);
    EXPECT_EQ(model_number.error().code, LlmBodyErrorCode::InvalidFieldType);
    EXPECT_EQ(model_number.error().field, "$.model");

    auto stream_string =
            ParsedLlmBody::parse(LlmWireProtocol::AnthropicMessages, make_body(R"({"stream":"yes"})"), pool);
    ASSERT_FALSE(stream_string);
    EXPECT_EQ(stream_string.error().code, LlmBodyErrorCode::InvalidFieldType);
    EXPECT_EQ(stream_string.error().field, "$.stream");
}

TEST(LlmBodyTest, RewritesOnlyExistingFieldsAndPreservesOtherBytes) {
    constexpr std::string_view input =
            R"({ "unknown" : [1e+09, "\u0041"], "model" : "public", "stream" : false, "tail":null })";
    BufPool pool;
    IoBufNodePool nodes;
    auto body = ParsedLlmBody::parse(LlmWireProtocol::OpenAiChatCompletions, make_body(input), pool);
    ASSERT_TRUE(body) << body.error().message;

    auto rewritten = body->rewrite("upstream/\"model", true, nodes);

    ASSERT_TRUE(rewritten) << rewritten.error().message;
    EXPECT_EQ(flatten(*rewritten),
              R"({ "unknown" : [1e+09, "\u0041"], "model" : "upstream/\"model", "stream" : true, "tail":null })");
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(body->raw_body().readable_data()),
                               body->raw_body().readable()),
              input);
}

TEST(LlmBodyTest, MissingStreamRemainsAbsent) {
    BufPool pool;
    IoBufNodePool nodes;
    auto body = ParsedLlmBody::parse(LlmWireProtocol::OpenAiChatCompletions,
                                     make_body(R"({"model":"public","temperature":0.25})"), pool);
    ASSERT_TRUE(body) << body.error().message;

    auto rewritten = body->rewrite("provider-model", true, nodes);

    ASSERT_TRUE(rewritten) << rewritten.error().message;
    EXPECT_EQ(flatten(*rewritten), R"({"model":"provider-model","temperature":0.25})");
}

TEST(LlmBodyTest, RewritesEveryDuplicateOccurrenceInInputOrder) {
    BufPool pool;
    IoBufNodePool nodes;
    auto body = ParsedLlmBody::parse(LlmWireProtocol::OpenAiChatCompletions,
                                     make_body(R"({"model":"a","stream":false,"model":"b","stream":true})"), pool);
    ASSERT_TRUE(body) << body.error().message;

    auto rewritten = body->rewrite("target", false, nodes);

    ASSERT_TRUE(rewritten) << rewritten.error().message;
    EXPECT_EQ(flatten(*rewritten), R"({"model":"target","stream":false,"model":"target","stream":false})");
}

TEST(LlmBodyTest, CreatesIndependentBodiesForProviderAttempts) {
    constexpr std::string_view input = R"({"model":"public","stream":true,"messages":[]})";
    BufPool pool;
    IoBufNodePool first_nodes;
    IoBufNodePool second_nodes;
    auto body = ParsedLlmBody::parse(LlmWireProtocol::OpenAiChatCompletions, make_body(input), pool);
    ASSERT_TRUE(body) << body.error().message;

    auto first = body->rewrite("provider-a", std::nullopt, first_nodes);
    auto second = body->rewrite("provider-b", false, second_nodes);

    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(flatten(*first), R"({"model":"provider-a","stream":true,"messages":[]})");
    EXPECT_EQ(flatten(*second), R"({"model":"provider-b","stream":false,"messages":[]})");
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(body->raw_body().readable_data()),
                               body->raw_body().readable()),
              input);
}

TEST(LlmBodyTest, RejectsInvalidUtf8Replacement) {
    BufPool pool;
    IoBufNodePool nodes;
    auto body = ParsedLlmBody::parse(LlmWireProtocol::OpenAiChatCompletions, make_body(R"({"model":"public"})"), pool);
    ASSERT_TRUE(body) << body.error().message;
    constexpr char invalid[] = {'x', static_cast<char>(0x80)};

    auto rewritten = body->rewrite(std::string_view(invalid, sizeof(invalid)), std::nullopt, nodes);

    ASSERT_FALSE(rewritten);
    EXPECT_EQ(rewritten.error().code, LlmBodyErrorCode::InvalidReplacement);
}

} // namespace
