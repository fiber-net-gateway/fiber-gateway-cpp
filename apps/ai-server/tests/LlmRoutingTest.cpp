#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "discovery/WeightedRendezvous.h"
#include "provider/ExecutionPlan.h"
#include "provider/ProviderEndpoint.h"
#include "provider/ProviderRuntime.h"
#include "routing/ModelAuthorization.h"
#include "routing/ProviderRouteKey.h"

namespace {

using namespace std::chrono_literals;
using fiber::ai_server::AuthorizedModel;
using fiber::ai_server::CompiledModelRoute;
using fiber::ai_server::ExecutionPlanErrorCode;
using fiber::ai_server::LlmConfigSnapshot;
using fiber::ai_server::LlmProjectSnapshot;
using fiber::ai_server::LlmRoutingData;
using fiber::ai_server::LlmWireProtocol;
using fiber::ai_server::ModelAuthorizationErrorCode;
using fiber::ai_server::ProjectProvider;
using fiber::ai_server::ProviderApiToken;
using fiber::ai_server::ProviderConfigSnapshot;
using fiber::ai_server::ProviderProtocol;
using fiber::ai_server::ProviderProtocolType;
using fiber::ai_server::ProviderRuntimeRegistry;
using fiber::ai_server::ProviderRuntimeState;
using fiber::json::JsonArray;
using fiber::json::Nullable;
using fiber::mem::BufPool;

std::shared_ptr<const ProjectProvider> make_provider(std::string name, std::vector<ProviderProtocolType> protocol_types,
                                                     std::vector<std::string> token_names = {"default"},
                                                     bool service_ready = true) {
    auto config = std::make_shared<ProviderConfigSnapshot>();
    config->name = name;
    config->base_url = "https://" + name + ".example.test";
    for (const std::string &token_name: token_names) {
        config->api_tokens.push_back(ProviderApiToken{
                .name = token_name,
                .token = "secret-" + token_name,
        });
    }
    for (ProviderProtocolType type: protocol_types) {
        config->protocols.push_back(ProviderProtocol{
                .type = type,
                .path = type == ProviderProtocolType::AnthropicMessages ? "/v1/messages" : "/v1/chat/completions",
                .model = name + "-model",
        });
    }
    auto provider = std::make_shared<ProjectProvider>();
    provider->name = std::move(name);
    provider->config = std::move(config);
    if (!service_ready) {
        auto mutable_config = std::const_pointer_cast<ProviderConfigSnapshot>(provider->config);
        mutable_config->base_url = "service://empty";
        provider->service = std::make_shared<fiber::ai_server::WeightedRendezvous>();
    }
    return provider;
}

std::shared_ptr<const LlmProjectSnapshot> make_project(CompiledModelRoute route,
                                                       std::vector<std::shared_ptr<const ProjectProvider>> providers) {
    return std::make_shared<LlmProjectSnapshot>(fiber::ai_server::ConfigMetadata{}, 1, std::move(providers),
                                                std::vector<CompiledModelRoute>{std::move(route)});
}

Nullable<std::string_view> text(std::string_view value) {
    Nullable<std::string_view> result;
    result.set_present(value);
    return result;
}

TEST(LlmRoutingTest, AuthorizesGroupMembersAndZhangwangBypass) {
    auto provider = make_provider("provider-a", {ProviderProtocolType::OpenAiChatCompletions});
    auto group = std::make_shared<fiber::ai_server::UserGroupSnapshot>();
    group->name = "staff";
    group->users = {"alice"};
    CompiledModelRoute route{
            .model_name = "chat.public",
            .providers = {provider},
            .allow_user_groups = {group},
    };
    LlmConfigSnapshot config{.project = make_project(std::move(route), {provider})};

    auto allowed = fiber::ai_server::authorize_model(config, "alice", "chat.public", nullptr);
    auto bypassed = fiber::ai_server::authorize_model(config, "zhangwang", "chat.public", nullptr);
    auto denied = fiber::ai_server::authorize_model(config, "mallory", "chat.public", nullptr);

    ASSERT_TRUE(allowed);
    EXPECT_EQ(allowed->model_name, "chat.public");
    ASSERT_TRUE(bypassed);
    EXPECT_EQ(bypassed->model_name, "chat.public");
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, ModelAuthorizationErrorCode::ModelNotAvailable);
}

TEST(LlmRoutingTest, ValidatesModelAndHidesMissingFromUnauthorized) {
    LlmConfigSnapshot unavailable;
    auto missing = fiber::ai_server::authorize_model(unavailable, "alice", "", nullptr);
    auto invalid = fiber::ai_server::authorize_model(unavailable, "alice", "../secret", nullptr);
    auto no_config = fiber::ai_server::authorize_model(unavailable, "alice", "chat", nullptr);

    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ModelAuthorizationErrorCode::ModelRequired);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ModelAuthorizationErrorCode::InvalidModelName);
    ASSERT_FALSE(no_config);
    EXPECT_EQ(no_config.error().code, ModelAuthorizationErrorCode::ModelConfigUnavailable);
}

TEST(LlmRoutingTest, BuildsProtocolSpecificRouteKeysAndHonorsUtf8ByteLimit) {
    Nullable<std::string_view> roles[] = {text("user"), text("assistant")};
    Nullable<std::string_view> contents[] = {text("你好吗"), text("ok")};
    LlmRoutingData routing;
    routing.model = text("chat");
    routing.message_roles = JsonArray<Nullable<std::string_view>>(roles, 2);
    routing.message_content_texts = JsonArray<Nullable<std::string_view>>(contents, 2);
    fiber::ai_server::LoadBalanceConfig config;
    config.prefix_max_bytes = 9;
    BufPool pool;

    auto key =
            fiber::ai_server::build_provider_route_key(LlmWireProtocol::OpenAiChatCompletions, routing, config, pool);

    ASSERT_TRUE(key);
    EXPECT_EQ(*key, "user:你\n");
    EXPECT_EQ(key->size(), 9u);

    routing.prompt_cache_key = text("cache-key");
    auto cached =
            fiber::ai_server::build_provider_route_key(LlmWireProtocol::OpenAiChatCompletions, routing, config, pool);
    ASSERT_TRUE(cached);
    EXPECT_EQ(*cached, "cache-key");

    routing.metadata_route_key = text("metadata-key");
    auto metadata =
            fiber::ai_server::build_provider_route_key(LlmWireProtocol::AnthropicMessages, routing, config, pool);
    ASSERT_TRUE(metadata);
    EXPECT_EQ(*metadata, "metadata-key");
}

TEST(LlmRoutingTest, UsesAnthropicContainerThenSystemAndMessages) {
    Nullable<std::string_view> roles[] = {text("user")};
    Nullable<std::string_view> contents[] = {text("hello")};
    LlmRoutingData routing;
    routing.model = text("claude");
    routing.container = text("session-1");
    routing.system_text = text("rules");
    routing.message_roles = JsonArray<Nullable<std::string_view>>(roles, 1);
    routing.message_content_texts = JsonArray<Nullable<std::string_view>>(contents, 1);
    fiber::ai_server::LoadBalanceConfig config;
    BufPool pool;

    auto container =
            fiber::ai_server::build_provider_route_key(LlmWireProtocol::AnthropicMessages, routing, config, pool);
    ASSERT_TRUE(container);
    EXPECT_EQ(*container, "session-1");

    routing.container.set_absent();
    auto prefix = fiber::ai_server::build_provider_route_key(LlmWireProtocol::AnthropicMessages, routing, config, pool);
    ASSERT_TRUE(prefix);
    EXPECT_EQ(*prefix, "rules\nuser:hello\n");
}

TEST(LlmRoutingTest, ProviderRuntimeAppliesTokenTtlAndThreeFailureCircuit) {
    ProviderRuntimeState state;
    const ProviderRuntimeState::TimePoint now{100s};

    state.mark_token_unavailable("token-a", now, 30s);
    EXPECT_FALSE(state.token_available("token-a", now + 29s));
    EXPECT_TRUE(state.token_available("token-a", now + 30s));

    state.record_provider_failure(now);
    state.record_provider_failure(now);
    EXPECT_TRUE(state.available(now));
    state.record_provider_failure(now);
    EXPECT_FALSE(state.available(now + 29s));
    EXPECT_TRUE(state.available(now + 30s));
}

TEST(LlmRoutingTest, ResolvesSameProtocolAndExpandsTokensInStableOrder) {
    auto openai =
            make_provider("openai-a", {ProviderProtocolType::OpenAiChatCompletions}, {"first", "second", "third"});
    auto anthropic = make_provider("anthropic-a", {ProviderProtocolType::AnthropicMessages});
    CompiledModelRoute route{
            .model_name = "chat",
            .providers = {openai, anthropic},
    };
    ProviderRuntimeRegistry runtime;
    BufPool first_pool;
    BufPool second_pool;
    const AuthorizedModel model{.model_name = "chat", .route = &route};
    const ProviderRuntimeState::TimePoint now{100s};

    auto first = fiber::ai_server::resolve_execution_plan(model, LlmWireProtocol::OpenAiChatCompletions, "same-key",
                                                          runtime, now, first_pool);
    auto second = fiber::ai_server::resolve_execution_plan(model, LlmWireProtocol::OpenAiChatCompletions, "same-key",
                                                           runtime, now, second_pool);

    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_EQ(first->attempts.size(), 3u);
    ASSERT_EQ(second->attempts.size(), 3u);
    for (std::size_t i = 0; i < first->attempts.size(); ++i) {
        EXPECT_EQ(first->attempts[i].provider->name, "openai-a");
        ASSERT_NE(first->attempts[i].api_token, nullptr);
        ASSERT_NE(second->attempts[i].api_token, nullptr);
        EXPECT_EQ(first->attempts[i].api_token->name, second->attempts[i].api_token->name);
    }
}

TEST(LlmRoutingTest, EnforcesPrimaryProviderLimitButKeepsSelectedProviderTokens) {
    auto first = make_provider("provider-a", {ProviderProtocolType::OpenAiChatCompletions}, {"a", "b"});
    auto second = make_provider("provider-b", {ProviderProtocolType::OpenAiChatCompletions}, {"a", "b"});
    auto fallback = make_provider("provider-f", {ProviderProtocolType::OpenAiChatCompletions}, {});
    CompiledModelRoute route{
            .model_name = "chat",
            .providers = {first, second},
            .fallback_provider = fallback,
    };
    route.load_balance.max_primary_attempts = 1;
    ProviderRuntimeRegistry runtime;
    BufPool pool;

    auto plan = fiber::ai_server::resolve_execution_plan(AuthorizedModel{.model_name = "chat", .route = &route},
                                                         LlmWireProtocol::OpenAiChatCompletions, {}, runtime,
                                                         ProviderRuntimeState::TimePoint{100s}, pool);

    ASSERT_TRUE(plan) << plan.error().message;
    ASSERT_EQ(plan->attempts.size(), 3u);
    EXPECT_EQ(plan->attempts[0].provider->name, "provider-a");
    EXPECT_EQ(plan->attempts[1].provider->name, "provider-a");
    EXPECT_FALSE(plan->attempts[0].fallback);
    EXPECT_TRUE(plan->attempts[2].fallback);
    EXPECT_EQ(plan->attempts[2].provider->name, "provider-f");
    EXPECT_EQ(plan->attempts[2].api_token, nullptr);
}

TEST(LlmRoutingTest, ReportsTokenProtocolAndServiceAvailabilityFailures) {
    const ProviderRuntimeState::TimePoint now{100s};
    ProviderRuntimeRegistry runtime;

    auto token_provider = make_provider("provider-token", {ProviderProtocolType::OpenAiChatCompletions}, {"only"});
    CompiledModelRoute token_route{.model_name = "chat", .providers = {token_provider}};
    runtime.state_for("provider-token").mark_token_unavailable("only", now, 1min);
    BufPool token_pool;
    auto token_error = fiber::ai_server::resolve_execution_plan(
            AuthorizedModel{.model_name = "chat", .route = &token_route}, LlmWireProtocol::OpenAiChatCompletions, {},
            runtime, now, token_pool);
    ASSERT_FALSE(token_error);
    EXPECT_EQ(token_error.error().code, ExecutionPlanErrorCode::ProviderTokenUnavailable);

    auto wrong_protocol = make_provider("anthropic", {ProviderProtocolType::AnthropicMessages});
    CompiledModelRoute protocol_route{.model_name = "chat", .providers = {wrong_protocol}};
    BufPool protocol_pool;
    auto protocol_error = fiber::ai_server::resolve_execution_plan(
            AuthorizedModel{.model_name = "chat", .route = &protocol_route}, LlmWireProtocol::OpenAiChatCompletions, {},
            runtime, now, protocol_pool);
    ASSERT_FALSE(protocol_error);
    EXPECT_EQ(protocol_error.error().code, ExecutionPlanErrorCode::ProviderProtocolUnsupported);

    auto empty_service = make_provider("service", {ProviderProtocolType::OpenAiChatCompletions}, {"token"}, false);
    CompiledModelRoute service_route{.model_name = "chat", .providers = {empty_service}};
    BufPool service_pool;
    auto service_error = fiber::ai_server::resolve_execution_plan(
            AuthorizedModel{.model_name = "chat", .route = &service_route}, LlmWireProtocol::OpenAiChatCompletions, {},
            runtime, now, service_pool);
    ASSERT_FALSE(service_error);
    EXPECT_EQ(service_error.error().code, ExecutionPlanErrorCode::ProviderConfigUnavailable);
}

TEST(LlmRoutingTest, ParsesProviderAddressAndServiceEndpointsStrictly) {
    auto https = fiber::ai_server::parse_provider_endpoint("https://[2001:db8::1]:8443/gateway");
    ASSERT_TRUE(https) << https.error().message;
    EXPECT_TRUE(https->tls());
    EXPECT_TRUE(https->host_is_ip);
    EXPECT_EQ(https->host, "2001:db8::1");
    EXPECT_EQ(https->port, 8443);
    EXPECT_EQ(https->base_path, "/gateway");

    auto service = fiber::ai_server::parse_provider_endpoint("service://llm.internal");
    ASSERT_TRUE(service) << service.error().message;
    EXPECT_TRUE(service->is_service());
    EXPECT_EQ(service->host, "llm.internal");

    EXPECT_FALSE(fiber::ai_server::parse_provider_endpoint("http://host:"));
    EXPECT_FALSE(fiber::ai_server::parse_provider_endpoint("https://user@host"));
    EXPECT_FALSE(fiber::ai_server::parse_provider_endpoint("http://bad host"));
    EXPECT_FALSE(fiber::ai_server::parse_provider_endpoint("service://name/path"));
}

} // namespace
