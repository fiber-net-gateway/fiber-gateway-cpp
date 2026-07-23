#include <gtest/gtest.h>

#include <chrono>
#include <string_view>

#include <event/EventLoop.h>
#include <event/EventLoopGroup.h>

#include "AiServerConfig.h"
#include "AiServerRuntime.h"

namespace {

using fiber::ai_server::AiServerConfig;
using fiber::ai_server::AiServerConfigErrorCode;

TEST(AiServerConfigTest, LoadsHttpAndNacosSettings) {
    constexpr std::string_view input = R"(
# ai-server process configuration
export AI_SERVER_LISTEN_ADDRESS=127.0.0.1
AI_SERVER_LISTEN_PORT=18080
NACOS_SERVER_ADDRESSES=127.0.0.1, [2001:db8::1], 127.0.0.1
NACOS_HTTP_PORT=18848
NACOS_GRPC_PORT=19848
NACOS_NAMESPACE_ID='llm-dev'
NACOS_TENANT=tenant-a
NACOS_USERNAME=nacos
NACOS_PASSWORD="pa\"ss"
NACOS_CONTEXT_PATH=/nacos/
NACOS_CLIENT_VERSION=fiber-ai-server/1.0 # inline comment
AI_SERVER_INITIAL_CONFIG_TIMEOUT_MS=15000
)";

    auto result = AiServerConfig::load_from_string(input);

    ASSERT_TRUE(result) << result.error().detail;
    EXPECT_EQ(result->listen_address().to_string(), "127.0.0.1:18080");
    const auto &nacos = result->nacos_config();
    ASSERT_EQ(nacos.server_ips().size(), 2u);
    EXPECT_EQ(nacos.server_ips()[0].to_string(), "127.0.0.1");
    EXPECT_EQ(nacos.server_ips()[1].to_string(), "2001:db8::1");
    EXPECT_EQ(nacos.http_port(), 18848);
    EXPECT_EQ(nacos.grpc_port(), 19848);
    EXPECT_EQ(nacos.namespace_id(), "llm-dev");
    EXPECT_EQ(nacos.tenant(), "tenant-a");
    EXPECT_EQ(nacos.username(), "nacos");
    EXPECT_EQ(nacos.password(), "pa\"ss");
    EXPECT_EQ(nacos.context_path(), "/nacos");
    EXPECT_EQ(nacos.client_version(), "fiber-ai-server/1.0");
    EXPECT_EQ(result->initial_config_timeout(), std::chrono::milliseconds(15000));
}

TEST(AiServerConfigTest, AppliesDefaultsForOptionalSettings) {
    auto result = AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n");

    ASSERT_TRUE(result) << result.error().detail;
    EXPECT_EQ(result->listen_address().to_string(), "0.0.0.0:8080");
    EXPECT_EQ(result->nacos_config().http_port(), 8848);
    EXPECT_EQ(result->nacos_config().grpc_port(), 9848);
    EXPECT_TRUE(result->nacos_config().username().empty());
    EXPECT_TRUE(result->nacos_config().password().empty());
    EXPECT_EQ(result->nacos_config().context_path(), "/nacos");
    EXPECT_EQ(result->initial_config_timeout(), std::chrono::milliseconds(60000));
}

TEST(AiServerConfigTest, RequiresNacosServerAddresses) {
    auto result = AiServerConfig::load_from_string("AI_SERVER_LISTEN_PORT=8080\n");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, AiServerConfigErrorCode::MissingRequiredKey);
    EXPECT_EQ(result.error().key, "NACOS_SERVER_ADDRESSES");
}

TEST(AiServerConfigTest, RejectsDuplicateAndUnknownKeys) {
    auto duplicate =
            AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\nNACOS_SERVER_ADDRESSES=127.0.0.2\n");
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, AiServerConfigErrorCode::DuplicateKey);
    EXPECT_EQ(duplicate.error().line, 2u);

    auto unknown = AiServerConfig::load_from_string("NACOS_SERVER_ADDRESS=127.0.0.1\n");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, AiServerConfigErrorCode::UnknownKey);
    EXPECT_EQ(unknown.error().key, "NACOS_SERVER_ADDRESS");
}

TEST(AiServerConfigTest, RejectsInvalidValuesAndPartialCredentials) {
    auto invalid_port =
            AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\nAI_SERVER_LISTEN_PORT=70000\n");
    ASSERT_FALSE(invalid_port);
    EXPECT_EQ(invalid_port.error().code, AiServerConfigErrorCode::InvalidValue);
    EXPECT_EQ(invalid_port.error().line, 2u);

    auto partial_credentials =
            AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\nNACOS_USERNAME=nacos\n");
    ASSERT_FALSE(partial_credentials);
    EXPECT_EQ(partial_credentials.error().code, AiServerConfigErrorCode::InvalidNacosConfig);
    EXPECT_EQ(partial_credentials.error().key, "NACOS_PASSWORD");

    auto invalid_timeout = AiServerConfig::load_from_string(
            "NACOS_SERVER_ADDRESSES=127.0.0.1\nAI_SERVER_INITIAL_CONFIG_TIMEOUT_MS=-1\n");
    ASSERT_FALSE(invalid_timeout);
    EXPECT_EQ(invalid_timeout.error().code, AiServerConfigErrorCode::InvalidValue);
    EXPECT_EQ(invalid_timeout.error().key, "AI_SERVER_INITIAL_CONFIG_TIMEOUT_MS");
}

TEST(AiServerConfigTest, ReportsMissingFiles) {
    auto result = AiServerConfig::load_from_file("/path/that/does/not/exist/ai-server.env");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, AiServerConfigErrorCode::OpenFailed);
}

TEST(AiServerConfigTest, LoadsExampleFile) {
    auto result = AiServerConfig::load_from_file(FIBER_AI_SERVER_TEST_ENV_PATH);

    ASSERT_TRUE(result) << result.error().detail;
    EXPECT_EQ(result->listen_address().to_string(), "0.0.0.0:8080");
    ASSERT_EQ(result->nacos_config().server_ips().size(), 1u);
    EXPECT_EQ(result->nacos_config().server_ips()[0].to_string(), "127.0.0.1");
    EXPECT_EQ(result->initial_config_timeout(), std::chrono::milliseconds(60000));
}

TEST(AiServerRuntimeTest, CreateDoesNotBindListener) {
    auto config = AiServerConfig::load_from_string(
            "NACOS_SERVER_ADDRESSES=127.0.0.1\nAI_SERVER_INITIAL_CONFIG_TIMEOUT_MS=0\n");
    ASSERT_TRUE(config);

    fiber::event::EventLoop accept_loop;
    fiber::event::EventLoop nacos_loop;
    fiber::event::EventLoop cat_loop;
    fiber::event::EventLoopGroup workers(1);
    auto runtime = fiber::ai_server::AiServerRuntime::create(accept_loop, nacos_loop, cat_loop, workers, *config);

    ASSERT_TRUE(runtime);
    EXPECT_EQ((*runtime)->state(), fiber::ai_server::AiServerRuntimeState::Created);
    EXPECT_LT((*runtime)->fd(), 0);
}

TEST(AiServerRuntimeTest, DefaultWorkerCountIsNeverZero) {
    EXPECT_GE(fiber::ai_server::default_http_worker_count(), 1u);
}

} // namespace
