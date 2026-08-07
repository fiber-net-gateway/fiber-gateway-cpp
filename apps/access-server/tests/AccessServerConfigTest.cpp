#include "../src/runtime/AccessServerConfig.h"

#include <gtest/gtest.h>

namespace fiber::access_server {
namespace {

TEST(AccessServerConfigTest, LoadsJavaServerDefaultsAndNacosSettings) {
    auto config = AccessServerConfig::load_from_string(R"(
        # Nacos is the only required process setting.
        NACOS_SERVER_ADDRESSES=127.0.0.1,127.0.0.2
    )");

    ASSERT_TRUE(config) << config.error().detail;
    EXPECT_EQ(config->listen_address().to_string(), "0.0.0.0:16688");
    EXPECT_EQ(config->metrics_listen_address().to_string(), "0.0.0.0:16689");
    EXPECT_EQ(config->initial_config_timeout(), std::chrono::seconds(60));
    EXPECT_EQ(config->default_max_request_body_size(), 400U << 20U);
    EXPECT_FALSE(config->test_mode());
    EXPECT_EQ(config->nacos_config().server_ips().size(), 2U);
    EXPECT_EQ(config->nacos_config().http_port(), 8848);
    EXPECT_EQ(config->nacos_config().grpc_port(), 9848);
    EXPECT_EQ(config->nacos_config().namespace_id(), "public");
    EXPECT_EQ(config->watcher_options().project_list_data_id, kProjectListDataId);
    EXPECT_EQ(config->watcher_options().project_route_data_id_prefix, kProjectRouteDataIdPrefix);
    EXPECT_EQ(config->watcher_options().project_route_group, kProjectRouteGroup);
    EXPECT_EQ(config->gray_watcher_options().data_id, kGrayConfigDataId);
    EXPECT_EQ(config->service_discovery_options().group, kDefaultNacosGroup);
}

TEST(AccessServerConfigTest, LoadsExplicitRuntimeAndCompatibilityKeys) {
    auto config = AccessServerConfig::load_from_string(R"(
        ACCESS_SERVER_LISTEN_ADDRESS=127.0.0.1
        ACCESS_SERVER_LISTEN_PORT=18080
        ACCESS_SERVER_METRICS_LISTEN_ADDRESS=127.0.0.2
        ACCESS_SERVER_METRICS_LISTEN_PORT=19090
        ACCESS_SERVER_INITIAL_CONFIG_TIMEOUT_MILLIS=2500
        ACCESS_SERVER_MAX_REQUEST_BODY_SIZE=12345
        ACCESS_SERVER_TEST_MODE=true
        ACCESS_SERVER_PROJECTS_DATA_ID=custom.projects
        ACCESS_SERVER_ROUTE_DATA_ID_PREFIX=custom.route.
        ACCESS_SERVER_ROUTE_GROUP=CUSTOM-ROUTE
        ACCESS_SERVER_GRAY_DATA_ID=custom.gray
        ACCESS_SERVER_NAMING_GROUP=CUSTOM-GROUP
        ACCESS_SERVER_ZONE=sh
        NACOS_SERVER_ADDRESSES=10.0.0.1
        NACOS_HTTP_PORT=18848
        NACOS_GRPC_PORT=19848
        NACOS_NAMESPACE=ns
        NACOS_TENANT=tenant
        NACOS_USERNAME=user
        NACOS_PASSWORD=pass
        NACOS_CLIENT_VERSION=access-test
    )");

    ASSERT_TRUE(config) << config.error().detail;
    EXPECT_EQ(config->listen_address().to_string(), "127.0.0.1:18080");
    EXPECT_EQ(config->metrics_listen_address().to_string(), "127.0.0.2:19090");
    EXPECT_EQ(config->initial_config_timeout(), std::chrono::milliseconds(2500));
    EXPECT_EQ(config->default_max_request_body_size(), 12345U);
    EXPECT_TRUE(config->test_mode());
    EXPECT_EQ(config->watcher_options().project_list_data_id, "custom.projects");
    EXPECT_EQ(config->watcher_options().project_route_data_id_prefix, "custom.route.");
    EXPECT_EQ(config->watcher_options().project_route_group, "CUSTOM-ROUTE");
    EXPECT_EQ(config->gray_watcher_options().data_id, "custom.gray");
    EXPECT_EQ(config->gray_watcher_options().group, "CUSTOM-GROUP");
    EXPECT_EQ(config->service_discovery_options().group, "CUSTOM-GROUP");
    EXPECT_EQ(config->service_discovery_options().zone, "sh");
    EXPECT_EQ(config->nacos_config().username(), "user");
    EXPECT_EQ(config->nacos_config().password(), "pass");
}

TEST(AccessServerConfigTest, LoadsOptionalCatClientSettings) {
    auto config = AccessServerConfig::load_from_string(R"(
        NACOS_SERVER_ADDRESSES=127.0.0.1
        CAT_APP_KEY=unified-access-server
        CAT_HOSTNAME=access-0
        CAT_IP=127.0.0.1
        CAT_ROUTER_ADDRESSES=127.0.0.2:8080
        CAT_COLLECTOR_ADDRESSES=127.0.0.3:2280
    )");

    ASSERT_TRUE(config) << config.error().detail;
    ASSERT_TRUE(config->cat_config());
    EXPECT_EQ(config->cat_config()->app_key(), "unified-access-server");
    EXPECT_EQ(config->cat_config()->hostname(), "access-0");
    EXPECT_EQ(config->cat_config()->ip(), "127.0.0.1");
    ASSERT_EQ(config->cat_config()->routers().size(), 1U);
    EXPECT_EQ(config->cat_config()->routers()[0].port, 8080);
    ASSERT_EQ(config->cat_config()->bootstrap_collectors().size(), 1U);
    EXPECT_EQ(config->cat_config()->bootstrap_collectors()[0].port(), 2280);
}

TEST(AccessServerConfigTest, RejectsPartialCatClientSettings) {
    auto config = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                       "CAT_APP_KEY=unified-access-server\n");

    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::InvalidValue);
    EXPECT_NE(config.error().detail.find("CAT_HOSTNAME"), std::string::npos);
}

TEST(AccessServerConfigTest, RejectsMissingNacosServerList) {
    auto config = AccessServerConfig::load_from_string("");

    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::MissingRequiredKey);
    EXPECT_EQ(config.error().key, "NACOS_SERVER_ADDRESSES");
}

TEST(AccessServerConfigTest, RejectsDuplicateAndUnknownKeys) {
    auto duplicate = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                          "NACOS_SERVER_ADDRESSES=127.0.0.2\n");
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, AccessServerConfigErrorCode::DuplicateKey);
    EXPECT_EQ(duplicate.error().line, 2U);

    auto unknown = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                        "ACCESS_SERVER_MAGIC=true\n");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, AccessServerConfigErrorCode::UnknownKey);
    EXPECT_EQ(unknown.error().line, 2U);
}

TEST(AccessServerConfigTest, RejectsRemovedHttpWorkerSetting) {
    auto config = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                       "ACCESS_SERVER_HTTP_WORKERS=4\n");

    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::UnknownKey);
    EXPECT_EQ(config.error().key, "ACCESS_SERVER_HTTP_WORKERS");
}

TEST(AccessServerConfigTest, RejectsRemovedDefaultUpstreamClusterSetting) {
    auto config = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                       "ACCESS_SERVER_DEFAULT_CLUSTER=stable\n");

    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::UnknownKey);
    EXPECT_EQ(config.error().key, "ACCESS_SERVER_DEFAULT_CLUSTER");
}

TEST(AccessServerConfigTest, RequiresCompleteNacosCredentials) {
    auto config = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                       "NACOS_USERNAME=user\n");

    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::InvalidNacosConfig);
}

TEST(AccessServerConfigTest, RejectsNonBooleanTestMode) {
    auto config = AccessServerConfig::load_from_string("NACOS_SERVER_ADDRESSES=127.0.0.1\n"
                                                       "ACCESS_SERVER_TEST_MODE=TRUE\n");

    ASSERT_FALSE(config);
    EXPECT_EQ(config.error().code, AccessServerConfigErrorCode::InvalidValue);
    EXPECT_EQ(config.error().key, "ACCESS_SERVER_TEST_MODE");
}

} // namespace
} // namespace fiber::access_server
