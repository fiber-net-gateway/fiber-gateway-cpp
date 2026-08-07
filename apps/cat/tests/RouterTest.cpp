#include <gtest/gtest.h>

#include <string>
#include <utility>

#include <fiber/cat/CatClientConfig.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>

#include "CatRouter.h"

namespace {

using fiber::cat::CatClientConfig;
using fiber::cat::CatClientConfigParams;
using fiber::cat::CatConfigError;
using fiber::cat::detail::RouterParseError;

TEST(CatRouterTest, ParsesOfficialKvsShapeAndDeduplicatesCollectors) {
    auto parsed = fiber::cat::detail::parse_router_response(
            R"({"kvs":{"routers":"127.0.0.1:2280;[::1]:2281;127.0.0.1:2280","sample":"0.5","block":"false"}})", 8);

    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->collectors.size(), 2);
    EXPECT_EQ(parsed->collectors[0].to_string(), "127.0.0.1:2280");
    EXPECT_EQ(parsed->collectors[1].to_string(), "[::1]:2281");
    EXPECT_DOUBLE_EQ(parsed->sample, 0.5);
    EXPECT_FALSE(parsed->block);
}

TEST(CatRouterTest, AcceptsNativeSampleAndBlockedEmptyRoute) {
    auto parsed = fiber::cat::detail::parse_router_response(
            R"({"ignored":1,"kvs":{"routers":"","sample":0.25,"block":true}})", 8);

    ASSERT_TRUE(parsed);
    EXPECT_TRUE(parsed->collectors.empty());
    EXPECT_DOUBLE_EQ(parsed->sample, 0.25);
    EXPECT_TRUE(parsed->block);
}

TEST(CatRouterTest, RejectsMalformedOrUnboundedCollectorLists) {
    auto hostname = fiber::cat::detail::parse_router_response(
            R"({"kvs":{"routers":"collector.example:2280","sample":1,"block":false}})", 8);
    ASSERT_FALSE(hostname);
    EXPECT_EQ(hostname.error(), RouterParseError::InvalidCollector);

    auto too_many = fiber::cat::detail::parse_router_response(
            R"({"kvs":{"routers":"127.0.0.1:1;127.0.0.2:2","sample":1,"block":false}})", 1);
    ASSERT_FALSE(too_many);
    EXPECT_EQ(too_many.error(), RouterParseError::TooManyCollectors);

    auto invalid_sample = fiber::cat::detail::parse_router_response(
            R"({"kvs":{"routers":"127.0.0.1:2280","sample":1.1,"block":false}})", 8);
    ASSERT_FALSE(invalid_sample);
    EXPECT_EQ(invalid_sample.error(), RouterParseError::InvalidResponse);
}

TEST(CatClientConfigTest, ValidatesRequiredIdentityAndServers) {
    CatClientConfigParams params;
    auto empty = CatClientConfig::create(params);
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error(), CatConfigError::EmptyAppKey);

    params.app_key = "checkout";
    params.hostname = "host-a";
    params.ip = "not-an-ip";
    auto invalid_ip = CatClientConfig::create(params);
    ASSERT_FALSE(invalid_ip);
    EXPECT_EQ(invalid_ip.error(), CatConfigError::InvalidIp);

    params.ip = "127.0.0.1";
    auto no_servers = CatClientConfig::create(params);
    ASSERT_FALSE(no_servers);
    EXPECT_EQ(no_servers.error(), CatConfigError::EmptyServerList);

    params.bootstrap_collectors.emplace_back(fiber::net::IpAddress::loopback_v4(), 2280);
    auto valid = CatClientConfig::create(std::move(params));
    ASSERT_TRUE(valid);
    EXPECT_EQ(valid->app_key(), "checkout");
}

} // namespace
