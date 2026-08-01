#include <gtest/gtest.h>

#include <string_view>

#include "config/LlmConfigCodec.h"

namespace {

using fiber::ai_server::LlmConfigErrorCode;
using fiber::ai_server::parse_bt1_key_config;
using fiber::ai_server::parse_models_config;
using fiber::ai_server::parse_provider_config;
using fiber::ai_server::parse_user_group_config;
using fiber::ai_server::ProviderProtocolType;

TEST(LlmConfigCodecTest, ParsesBt1EnvelopeAndDecodesSecrets) {
    constexpr std::string_view input = R"({
        "version": 7,
        "ignored": true,
        "data": {
            "clockSkewSec": 30,
            "keys": [
                {"kid": "key-b", "secret": "base64:c2VjcmV0"},
                {"kid": "key-a", "secret": "plain"}
            ]
        }
    })";

    auto result = parse_bt1_key_config(input, "bt1-md5");

    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->metadata.version, 7);
    EXPECT_EQ(result->metadata.md5, "bt1-md5");
    EXPECT_EQ(result->clock_skew_seconds, 30);
    ASSERT_EQ(result->keys.size(), 2u);
    EXPECT_EQ(result->keys[0].kid, "key-a");
    EXPECT_EQ(result->keys[0].secret, "plain");
    EXPECT_EQ(result->keys[1].kid, "key-b");
    EXPECT_EQ(result->keys[1].secret, "secret");
    ASSERT_NE(result->find_key("key-b"), nullptr);
    EXPECT_EQ(result->find_key("key-b")->secret, "secret");
}

TEST(LlmConfigCodecTest, RejectsDuplicateBt1KeysAndInvalidBase64) {
    auto duplicate = parse_bt1_key_config(
            R"({"data":{"keys":[{"kid":"same","secret":"one"},{"kid":"same","secret":"two"}]}})", "duplicate");
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, LlmConfigErrorCode::DuplicateValue);

    auto invalid = parse_bt1_key_config(R"({"data":{"keys":[{"kid":"key","secret":"base64:%%%"}]}})", "invalid");
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().field, "data.keys[0].secret");
}

TEST(LlmConfigCodecTest, ParsesProviderAliasesAndAllowsNoApiTokens) {
    constexpr std::string_view input = R"({
        "version": 3,
        "data": {
            "provider": "openai",
            "baseUrl": "https://api.example.test///",
            "apiTokens": [],
            "protocols": [{
                "type": "openai-chat-completions",
                "path": "/v1/chat/completions",
                "model": "gpt-test"
            }]
        }
    })";

    auto result = parse_provider_config(input, "provider-md5", "openai");

    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->base_url, "https://api.example.test");
    EXPECT_TRUE(result->api_tokens.empty());
    const auto *protocol = result->find_protocol(ProviderProtocolType::OpenAiChatCompletions);
    ASSERT_NE(protocol, nullptr);
    EXPECT_EQ(protocol->path, "/v1/chat/completions");
    EXPECT_EQ(protocol->model, "gpt-test");
}

TEST(LlmConfigCodecTest, ParsesModelsWithJavaDefaultsAndAliases) {
    constexpr std::string_view input = R"({
        "version": -2,
        "data": [{
            "modelName": "chat.main",
            "providers": ["openai"],
            "fallbackProvider": "backup",
            "allowUserGroups": ["staff", "staff"],
            "loadBalance": {
                "serviceInstancePolicy": " weighted-rendezvous-hash ",
                "prefixMaxBytes": 4096,
                "maxPrimaryAttempts": 2,
                "fallbackEnabled": false,
                "retryableStatus": [503, 429, 503]
            },
            "rateLimit": {
                "windowDurationMillis": 60000,
                "maxTokensPerWindow": 1000
            }
        }]
    })";

    auto result = parse_models_config(input, "models-md5");

    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->metadata.version, -2);
    ASSERT_EQ(result->models.size(), 1u);
    const auto &model = result->models[0];
    EXPECT_EQ(model.model_name, "chat.main");
    EXPECT_EQ(model.providers, std::vector<std::string>({"openai"}));
    ASSERT_TRUE(model.fallback_provider);
    EXPECT_EQ(*model.fallback_provider, "backup");
    EXPECT_EQ(model.allow_user_groups, std::vector<std::string>({"staff"}));
    EXPECT_EQ(model.load_balance.prefix_max_bytes, 4096);
    EXPECT_EQ(model.load_balance.max_primary_attempts, 2);
    EXPECT_FALSE(model.load_balance.fallback_enabled);
    EXPECT_EQ(model.load_balance.retryable_statuses, std::vector<std::int32_t>({429, 503}));
    ASSERT_TRUE(model.rate_limit);
    EXPECT_EQ(model.rate_limit->window_duration_millis, 60000);
    EXPECT_EQ(model.rate_limit->max_tokens_per_window, 1000);
}

TEST(LlmConfigCodecTest, ValidatesDynamicConfigNamesAndRelationships) {
    auto wrong_group = parse_user_group_config(R"({"data":{"name":"other","users":["alice"]}})", "group-md5", "staff");
    ASSERT_FALSE(wrong_group);
    EXPECT_EQ(wrong_group.error().field, "data.name");

    auto duplicate_fallback = parse_models_config(
            R"({"data":[{"model-name":"chat","providers":["openai"],"fallback-provider":"openai"}]})", "models-md5");
    ASSERT_FALSE(duplicate_fallback);
    EXPECT_EQ(duplicate_fallback.error().code, LlmConfigErrorCode::DuplicateValue);

    auto invalid_service_policy = parse_models_config(
            R"({"data":[{"model-name":"chat","providers":["openai"],"load-balance":{"service-instance-policy":"random"}}]})",
            "models-md5");
    ASSERT_FALSE(invalid_service_policy);
    EXPECT_EQ(invalid_service_policy.error().field, "data[0].load-balance.service-instance-policy");

    auto swrr = parse_models_config(
            R"({"data":[{"model-name":"chat","providers":["openai"],"load-balance":{"service-instance-policy":"smooth-weighted-round-robin"}}]})",
            "models-md5");
    ASSERT_FALSE(swrr);
    EXPECT_EQ(swrr.error().field, "data[0].load-balance.service-instance-policy");
}

} // namespace
