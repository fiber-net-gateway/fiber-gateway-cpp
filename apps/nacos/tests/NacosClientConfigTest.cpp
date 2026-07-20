#include <gtest/gtest.h>

#include <string>
#include <utility>

#include <fiber/nacos/NacosClientConfig.h>

namespace {

fiber::net::IpAddress parse_ip(std::string_view text) {
    fiber::net::IpAddress ip;
    EXPECT_TRUE(fiber::net::IpAddress::parse(text, ip));
    return ip;
}

fiber::nacos::NacosClientConfigParams valid_params() {
    fiber::nacos::NacosClientConfigParams params;
    params.server_ips.push_back(parse_ip("127.0.0.1"));
    params.username = "user";
    params.password = "password";
    return params;
}

TEST(NacosClientConfigTest, AppliesDefaultsAndOwnsValues) {
    auto params = valid_params();
    params.namespace_id = "namespace";
    params.tenant = "tenant";

    auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->server_ips().size(), 1u);
    EXPECT_EQ(result->username(), "user");
    EXPECT_EQ(result->password(), "password");
    EXPECT_EQ(result->http_port(), 8848);
    EXPECT_EQ(result->grpc_port(), 9848);
    EXPECT_EQ(result->namespace_id(), "namespace");
    EXPECT_EQ(result->tenant(), "tenant");
    EXPECT_EQ(result->client_version(), "fiber-nacos/1.0");
    EXPECT_EQ(result->context_path(), "/nacos");
}

TEST(NacosClientConfigTest, ClientOptionsEnableGrpcTcpNoDelayByDefault) {
    fiber::nacos::NacosClientOptions options;
    EXPECT_EQ(options.grpc_tcp.no_delay, fiber::net::TcpOptionMode::Enabled);
}

TEST(NacosClientConfigTest, AllowsAuthenticationToBeUnconfigured) {
    auto params = valid_params();
    params.username.clear();
    params.password.clear();

    auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->username().empty());
    EXPECT_TRUE(result->password().empty());
}

TEST(NacosClientConfigTest, RejectsInvalidRequiredFields) {
    {
        auto params = valid_params();
        params.server_ips.clear();
        auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, fiber::nacos::NacosConfigErrorCode::EmptyServerList);
    }
    {
        auto params = valid_params();
        params.server_ips[0] = fiber::net::IpAddress::any_v4();
        auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, fiber::nacos::NacosConfigErrorCode::InvalidServerAddress);
        EXPECT_EQ(result.error().server_index, 0u);
    }
    {
        auto params = valid_params();
        params.http_port = 0;
        auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, fiber::nacos::NacosConfigErrorCode::InvalidHttpPort);
    }
    {
        auto params = valid_params();
        params.grpc_port = 0;
        auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, fiber::nacos::NacosConfigErrorCode::InvalidGrpcPort);
    }
    {
        auto params = valid_params();
        params.username.clear();
        auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, fiber::nacos::NacosConfigErrorCode::EmptyUsername);
    }
    {
        auto params = valid_params();
        params.password.clear();
        auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, fiber::nacos::NacosConfigErrorCode::EmptyPassword);
    }
}

TEST(NacosClientConfigTest, NormalizesContextPathAndDeduplicatesServers) {
    auto params = valid_params();
    params.server_ips.push_back(parse_ip("127.0.0.1"));
    params.server_ips.push_back(parse_ip("127.0.0.2"));
    params.context_path = "/custom///";

    auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->server_ips().size(), 2u);
    EXPECT_EQ(result->server_ips()[0], parse_ip("127.0.0.1"));
    EXPECT_EQ(result->server_ips()[1], parse_ip("127.0.0.2"));
    EXPECT_EQ(result->context_path(), "/custom");
}

TEST(NacosClientConfigTest, RejectsMalformedContextPath) {
    for (const std::string path: {"", "nacos", "/nacos?x=1", "/nacos#fragment", "/nacos\r\nx"}) {
        auto params = valid_params();
        params.context_path = path;
        auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
        ASSERT_FALSE(result.has_value()) << path;
        EXPECT_EQ(result.error().code, fiber::nacos::NacosConfigErrorCode::InvalidContextPath);
    }
}

} // namespace
