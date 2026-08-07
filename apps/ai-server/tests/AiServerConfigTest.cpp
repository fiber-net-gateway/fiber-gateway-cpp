#include <gtest/gtest.h>

#include <chrono>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>

#include "AiServerConfig.h"
#include "AiServerRuntime.h"

namespace {

using fiber::ai_server::AiServerConfig;
using fiber::ai_server::AiServerConfigErrorCode;

std::expected<fiber::net::LocalIpv4Selection, fiber::net::LocalIpv4Error> detect_test_local_ipv4() noexcept {
    return fiber::net::LocalIpv4Selection{
            .address = fiber::net::IpAddress::v4({10, 20, 30, 40}),
            .interface_index = 7,
    };
}

std::expected<fiber::net::LocalIpv4Selection, fiber::net::LocalIpv4Error> fail_test_local_ipv4() noexcept {
    return std::unexpected(fiber::net::LocalIpv4Error{
            .code = fiber::net::LocalIpv4ErrorCode::NotFound,
    });
}

TEST(AiServerConfigTest, LoadsHttpAndNacosSettings) {
    constexpr std::string_view input = R"(
# ai-server process configuration
export AI_SERVER_LISTEN_ADDRESS=127.0.0.1
AI_SERVER_LISTEN_PORT=18080
AI_SERVER_ADVERTISE_ADDRESS=127.0.0.2
AI_SERVER_SERVICE_NAME=custom-ai-server
AI_SERVER_SERVICE_GROUP=AI_GROUP
AI_SERVER_ZONE=daily1
AI_SERVER_CLUSTER=gray
NACOS_SERVER_ADDRESSES=127.0.0.1, [2001:db8::1], 127.0.0.1
NACOS_HTTP_PORT=18848
NACOS_GRPC_PORT=19848
NACOS_NAMESPACE_ID='llm-dev'
NACOS_TENANT=tenant-a
NACOS_USERNAME=nacos
NACOS_PASSWORD="pa\"ss"
NACOS_CLIENT_VERSION=fiber-ai-server/1.0 # inline comment
CAT_APP_KEY=ploto-ai-server
CAT_HOSTNAME=ai-host-1
CAT_IP=127.0.0.3
CAT_ROUTER_ADDRESSES=127.0.0.10:8080,[2001:db8::10]:8081
CAT_COLLECTOR_ADDRESSES=127.0.0.11:2280
AI_SERVER_LOG_CONFIG_PATH=/tmp/custom-ai-server.logging.json
AI_SERVER_INITIAL_CONFIG_TIMEOUT_MS=15000
)";

    auto result = AiServerConfig::load_from_string(input);

    ASSERT_TRUE(result) << result.error().detail;
    EXPECT_EQ(result->listen_address().to_string(), "127.0.0.1:18080");
    EXPECT_EQ(result->advertise_address().to_string(), "127.0.0.2");
    EXPECT_FALSE(result->detected_local_ipv4());
    EXPECT_EQ(result->service_name(), "custom-ai-server");
    EXPECT_EQ(result->service_group(), "AI_GROUP");
    EXPECT_EQ(result->zone(), "daily1");
    EXPECT_EQ(result->cluster(), "gray");
    EXPECT_EQ(result->nacos_cluster(), "daily1-gray");
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
    EXPECT_EQ(nacos.client_version(), "fiber-ai-server/1.0");
    EXPECT_EQ(result->initial_config_timeout(), std::chrono::milliseconds(15000));
    ASSERT_TRUE(result->cat_config());
    EXPECT_EQ(result->cat_config()->app_key(), "ploto-ai-server");
    EXPECT_EQ(result->cat_config()->hostname(), "ai-host-1");
    EXPECT_EQ(result->cat_config()->ip(), "127.0.0.3");
    ASSERT_EQ(result->cat_config()->routers().size(), 2u);
    EXPECT_EQ(result->cat_config()->routers()[1].host, "2001:db8::10");
    ASSERT_EQ(result->cat_config()->bootstrap_collectors().size(), 1u);
    EXPECT_EQ(result->cat_config()->bootstrap_collectors()[0].to_string(), "127.0.0.11:2280");
    EXPECT_EQ(result->logging_config_path(), "/tmp/custom-ai-server.logging.json");
}

TEST(AiServerConfigTest, AppliesDefaultsForOptionalSettings) {
    auto result = AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                   "AI_SERVER_LOG_CONFIG_PATH=ai-server.logging.json\n",
                                                   detect_test_local_ipv4);

    ASSERT_TRUE(result) << result.error().detail;
    EXPECT_EQ(result->listen_address().to_string(), "0.0.0.0:8080");
    EXPECT_EQ(result->nacos_config().http_port(), 8848);
    EXPECT_EQ(result->nacos_config().grpc_port(), 9848);
    EXPECT_EQ(result->nacos_config().namespace_id(), "public");
    EXPECT_TRUE(result->nacos_config().tenant().empty());
    EXPECT_TRUE(result->nacos_config().username().empty());
    EXPECT_TRUE(result->nacos_config().password().empty());
    EXPECT_EQ(result->initial_config_timeout(), std::chrono::milliseconds(60000));
    EXPECT_EQ(result->advertise_address().to_string(), "10.20.30.40");
    ASSERT_TRUE(result->detected_local_ipv4());
    EXPECT_EQ(result->detected_local_ipv4()->interface_index, 7u);
    EXPECT_EQ(result->service_name(), "fiber-ai-server");
    EXPECT_EQ(result->service_group(), "DEFAULT_GROUP");
    EXPECT_EQ(result->zone(), "daily1");
    EXPECT_EQ(result->cluster(), "dev");
    EXPECT_EQ(result->nacos_cluster(), "daily1-dev");
    EXPECT_FALSE(result->cat_config());
    EXPECT_EQ(result->logging_config_path(), "ai-server.logging.json");
}

TEST(AiServerConfigTest, UsesResolvedHostIpv4ForNacosAndCatDefaults) {
    auto result = AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                   "AI_SERVER_LOG_CONFIG_PATH=ai-server.logging.json\n"
                                                   "CAT_APP_KEY=ploto-ai-server\n"
                                                   "CAT_HOSTNAME=host-a\n"
                                                   "CAT_ROUTER_ADDRESSES=127.0.0.10:8080\n",
                                                   detect_test_local_ipv4);

    ASSERT_TRUE(result) << result.error().detail;
    EXPECT_EQ(result->advertise_address().to_string(), "10.20.30.40");
    ASSERT_TRUE(result->cat_config());
    EXPECT_EQ(result->cat_config()->ip(), "10.20.30.40");
}

TEST(AiServerConfigTest, ConcreteListenAddressAvoidsInterfaceDetection) {
    auto result = AiServerConfig::load_from_string("AI_SERVER_LISTEN_ADDRESS=127.0.0.2\n"
                                                   "NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                   "AI_SERVER_LOG_CONFIG_PATH=ai-server.logging.json\n"
                                                   "CAT_APP_KEY=ploto-ai-server\n"
                                                   "CAT_HOSTNAME=host-a\n"
                                                   "CAT_ROUTER_ADDRESSES=127.0.0.10:8080\n",
                                                   fail_test_local_ipv4);

    ASSERT_TRUE(result) << result.error().detail;
    EXPECT_EQ(result->advertise_address().to_string(), "127.0.0.2");
    EXPECT_FALSE(result->detected_local_ipv4());
    ASSERT_TRUE(result->cat_config());
    EXPECT_EQ(result->cat_config()->ip(), "127.0.0.2");
}

TEST(AiServerConfigTest, ReportsMissingUsableLocalIpv4) {
    auto result = AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                   "AI_SERVER_LOG_CONFIG_PATH=ai-server.logging.json\n",
                                                   fail_test_local_ipv4);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, AiServerConfigErrorCode::LocalAddressUnavailable);
    EXPECT_EQ(result.error().key, "AI_SERVER_ADVERTISE_ADDRESS");
}

TEST(AiServerConfigTest, RequiresNacosServerAddresses) {
    auto result = AiServerConfig::load_from_string("AI_SERVER_LISTEN_PORT=8080\n"
                                                   "AI_SERVER_LOG_CONFIG_PATH=ai-server.logging.json\n");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, AiServerConfigErrorCode::MissingRequiredKey);
    EXPECT_EQ(result.error().key, "NACOS_SERVER_ADDRESSES");
}

TEST(AiServerConfigTest, RequiresLoggingConfigPath) {
    auto result = AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, AiServerConfigErrorCode::MissingRequiredKey);
    EXPECT_EQ(result.error().key, "AI_SERVER_LOG_CONFIG_PATH");
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

    auto legacy_audit = AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                         "AI_SERVER_LOG_CONFIG_PATH=ai-server.logging.json\n"
                                                         "AI_SERVER_AUDIT_LOG_PATH=ai-server-audit.ndjson\n");
    ASSERT_FALSE(legacy_audit);
    EXPECT_EQ(legacy_audit.error().code, AiServerConfigErrorCode::UnknownKey);
    EXPECT_EQ(legacy_audit.error().key, "AI_SERVER_AUDIT_LOG_PATH");
}

TEST(AiServerConfigTest, RejectsConfigurableNacosContextPath) {
    auto result = AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                   "NACOS_CONTEXT_PATH=/custom\n"
                                                   "AI_SERVER_LOG_CONFIG_PATH=ai-server.logging.json\n");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, AiServerConfigErrorCode::UnknownKey);
    EXPECT_EQ(result.error().key, "NACOS_CONTEXT_PATH");
}

TEST(AiServerConfigTest, RejectsInvalidValuesAndPartialCredentials) {
    auto invalid_port =
            AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\nAI_SERVER_LISTEN_PORT=70000\n");
    ASSERT_FALSE(invalid_port);
    EXPECT_EQ(invalid_port.error().code, AiServerConfigErrorCode::InvalidValue);
    EXPECT_EQ(invalid_port.error().line, 2u);

    auto partial_credentials = AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                                "AI_SERVER_LOG_CONFIG_PATH=ai-server.logging.json\n"
                                                                "NACOS_USERNAME=nacos\n");
    ASSERT_FALSE(partial_credentials);
    EXPECT_EQ(partial_credentials.error().code, AiServerConfigErrorCode::InvalidNacosConfig);
    EXPECT_EQ(partial_credentials.error().key, "NACOS_PASSWORD");

    auto invalid_timeout = AiServerConfig::load_from_string(
            "NACOS_SERVER_ADDRESSES=127.0.0.1\nAI_SERVER_INITIAL_CONFIG_TIMEOUT_MS=-1\n");
    ASSERT_FALSE(invalid_timeout);
    EXPECT_EQ(invalid_timeout.error().code, AiServerConfigErrorCode::InvalidValue);
    EXPECT_EQ(invalid_timeout.error().key, "AI_SERVER_INITIAL_CONFIG_TIMEOUT_MS");

    auto invalid_advertise = AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                              "AI_SERVER_ADVERTISE_ADDRESS=::1\n");
    ASSERT_FALSE(invalid_advertise);
    EXPECT_EQ(invalid_advertise.error().code, AiServerConfigErrorCode::InvalidValue);

    auto partial_cat = AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                        "AI_SERVER_LOG_CONFIG_PATH=ai-server.logging.json\n"
                                                        "CAT_APP_KEY=ploto-ai-server\n"
                                                        "CAT_ROUTER_ADDRESSES=127.0.0.10:8080\n",
                                                        detect_test_local_ipv4);
    ASSERT_FALSE(partial_cat);
    EXPECT_EQ(partial_cat.error().code, AiServerConfigErrorCode::MissingRequiredKey);
    EXPECT_EQ(partial_cat.error().key, "CAT_HOSTNAME");

    auto invalid_cat_endpoint = AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                                 "AI_SERVER_LOG_CONFIG_PATH=ai-server.logging.json\n"
                                                                 "CAT_APP_KEY=ploto-ai-server\n"
                                                                 "CAT_HOSTNAME=host-a\n"
                                                                 "CAT_IP=127.0.0.1\n"
                                                                 "CAT_ROUTER_ADDRESSES=localhost:8080\n");
    ASSERT_FALSE(invalid_cat_endpoint);
    EXPECT_EQ(invalid_cat_endpoint.error().code, AiServerConfigErrorCode::InvalidValue);
}

TEST(AiServerConfigTest, RejectsInvalidNacosClusterComponents) {
    auto empty_zone = AiServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                       "AI_SERVER_ZONE=\n"
                                                       "AI_SERVER_LOG_CONFIG_PATH=ai-server.logging.json\n");
    ASSERT_FALSE(empty_zone);
    EXPECT_EQ(empty_zone.error().code, AiServerConfigErrorCode::InvalidValue);
    EXPECT_EQ(empty_zone.error().key, "AI_SERVER_ZONE");

    const std::string oversized = "NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                  "AI_SERVER_ZONE=" +
                                  std::string(200, 'z') +
                                  "\n"
                                  "AI_SERVER_CLUSTER=" +
                                  std::string(100, 'c') +
                                  "\n"
                                  "AI_SERVER_LOG_CONFIG_PATH=ai-server.logging.json\n";
    auto oversized_cluster = AiServerConfig::load_from_string(oversized);
    ASSERT_FALSE(oversized_cluster);
    EXPECT_EQ(oversized_cluster.error().code, AiServerConfigErrorCode::InvalidValue);
    EXPECT_EQ(oversized_cluster.error().key, "AI_SERVER_CLUSTER");
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
    const auto expected_logging_path =
            (std::filesystem::path(FIBER_AI_SERVER_TEST_ENV_PATH).parent_path() / "ai-server.logging.json")
                    .lexically_normal();
    EXPECT_EQ(result->logging_config_path(), expected_logging_path.string());
}

TEST(AiServerRuntimeTest, CreateDoesNotBindListener) {
    const std::string config_text = "NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                    "AI_SERVER_INITIAL_CONFIG_TIMEOUT_MS=0\n"
                                    "AI_SERVER_LOG_CONFIG_PATH=ai-server.logging.json\n";
    auto config = AiServerConfig::load_from_string(config_text, detect_test_local_ipv4);
    ASSERT_TRUE(config);

    fiber::event::EventLoop accept_loop;
    fiber::event::EventLoop nacos_loop;
    fiber::event::EventLoop cat_loop;
    fiber::event::EventLoopGroup workers(1);
    auto runtime = fiber::ai_server::AiServerRuntime::create(accept_loop, nacos_loop, cat_loop, workers, *config,
                                                             128 * 1024 * 1024, fiber::log::kInvalidAppenderId);

    ASSERT_TRUE(runtime);
    EXPECT_EQ((*runtime)->state(), fiber::ai_server::AiServerRuntimeState::Created);
    EXPECT_LT((*runtime)->fd(), 0);
    runtime->reset();
}

} // namespace
