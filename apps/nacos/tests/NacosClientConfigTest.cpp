#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include <fiber/nacos/NacosClientConfig.h>
#include <fiber/nacos/NacosRpcOptions.h>

namespace {

fiber::nacos::NacosClientConfigParams valid_params() {
    fiber::nacos::NacosClientConfigParams params;
    params.server_hosts.push_back("127.0.0.1");
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
    EXPECT_EQ(result->server_hosts().size(), 1u);
    EXPECT_EQ(result->server_hosts()[0].value(), "127.0.0.1");
    EXPECT_TRUE(result->server_hosts()[0].is_ip_literal());
    EXPECT_FALSE(result->has_hostname_server());
    EXPECT_EQ(result->username(), "user");
    EXPECT_EQ(result->password(), "password");
    EXPECT_EQ(result->http_port(), 8848);
    EXPECT_EQ(result->grpc_port(), 9848);
    EXPECT_EQ(result->namespace_id(), "namespace");
    EXPECT_EQ(result->tenant(), "tenant");
    EXPECT_EQ(result->client_version(), "fiber-nacos/1.0");
}

TEST(NacosClientConfigTest, RpcOptionsEnableTcpNoDelayByDefault) {
    fiber::nacos::NacosRpcOptions options;
    EXPECT_EQ(options.tcp.no_delay, fiber::net::TcpOptionMode::Enabled);
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
        params.server_hosts.clear();
        auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, fiber::nacos::NacosConfigErrorCode::EmptyServerList);
    }
    {
        auto params = valid_params();
        params.server_hosts[0] = "0.0.0.0";
        auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, fiber::nacos::NacosConfigErrorCode::InvalidServerHost);
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

TEST(NacosClientConfigTest, DeduplicatesServers) {
    auto params = valid_params();
    params.server_hosts.push_back("127.0.0.1");
    params.server_hosts.push_back("NACOS.Internal.");
    params.server_hosts.push_back("nacos.internal");
    params.server_hosts.push_back("127.0.0.2");

    auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->server_hosts().size(), 3u);
    EXPECT_EQ(result->server_hosts()[0].value(), "127.0.0.1");
    EXPECT_EQ(result->server_hosts()[1].value(), "nacos.internal");
    EXPECT_EQ(result->server_hosts()[2].value(), "127.0.0.2");
    EXPECT_FALSE(result->server_hosts()[1].is_ip_literal());
    EXPECT_TRUE(result->has_hostname_server());
}

TEST(NacosClientConfigTest, RejectsInvalidServerHosts) {
    const std::vector<std::string> invalid_hosts{
            "",
            ".",
            "http://nacos.internal",
            "nacos.internal:8848",
            "-nacos.internal",
            "nacos-.internal",
            "nacos..internal",
            "nacos.internal..",
            "224.0.0.1",
            "::",
    };
    for (const std::string &host: invalid_hosts) {
        SCOPED_TRACE(host);
        auto params = valid_params();
        params.server_hosts[0] = host;
        auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, fiber::nacos::NacosConfigErrorCode::InvalidServerHost);
        EXPECT_EQ(result.error().server_index, 0u);
    }
}

TEST(NacosClientConfigTest, AcceptsIpv6AndInternalHostnameForms) {
    auto params = valid_params();
    params.server_hosts = {"2001:db8::1", "_nacos._tcp.internal"};

    auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->server_hosts().size(), 2u);
    EXPECT_EQ(result->server_hosts()[0].value(), "2001:db8::1");
    EXPECT_TRUE(result->server_hosts()[0].is_ip_literal());
    EXPECT_EQ(result->server_hosts()[1].value(), "_nacos._tcp.internal");
    EXPECT_FALSE(result->server_hosts()[1].is_ip_literal());
}

} // namespace
