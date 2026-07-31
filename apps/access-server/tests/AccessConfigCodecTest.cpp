#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/AccessConfigCodec.h"

namespace {

using fiber::access_server::AccessConfigErrorCode;
using fiber::access_server::BodyType;
using fiber::access_server::HostConfigEntry;
using fiber::access_server::HttpsStrategy;
using fiber::access_server::kNetOffice;
using fiber::access_server::kNetVdi;
using fiber::access_server::parse_project_config;
using fiber::access_server::parse_project_list;
using fiber::access_server::ProjectConfig;
using fiber::access_server::RouteConfig;
using fiber::access_server::RouteType;
using fiber::access_server::StringConfigEntry;

std::string read_fixture(std::string_view name) {
    std::string path = FIBER_ACCESS_SERVER_TEST_FIXTURE_DIR;
    path.push_back('/');
    path.append(name);
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

const HostConfigEntry *find_host(const ProjectConfig &config, std::string_view pattern) {
    if (!config.hosts) {
        return nullptr;
    }
    for (const HostConfigEntry &host: *config.hosts) {
        if (host.pattern == pattern) {
            return &host;
        }
    }
    return nullptr;
}

const StringConfigEntry *find_string_entry(const std::vector<StringConfigEntry> &entries, std::string_view name) {
    for (const StringConfigEntry &entry: entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

bool contains_nullable_string(const std::vector<std::optional<std::string>> &values,
                              const std::optional<std::string> &expected) {
    return std::find(values.begin(), values.end(), expected) != values.end();
}

const RouteConfig &require_route(const ProjectConfig &config, std::size_t index) {
    EXPECT_TRUE(config.routes);
    EXPECT_LT(index, config.routes ? config.routes->size() : 0);
    EXPECT_TRUE(config.routes && index < config.routes->size() && (*config.routes)[index]);
    return *(*config.routes)[index];
}

TEST(AccessConfigCodecTest, ParsesFullJavaProjectConfiguration) {
    const std::string input = read_fixture("project-conf-full.json");
    ASSERT_FALSE(input.empty());

    auto result = parse_project_config(input);

    ASSERT_TRUE(result) << result.error().message;
    ASSERT_TRUE(*result);
    const ProjectConfig &config = **result;
    EXPECT_EQ(config.version, 12);
    ASSERT_TRUE(config.hosts);
    EXPECT_EQ(config.hosts->size(), 2U);

    const HostConfigEntry *api_host = find_host(config, "api.example.com");
    ASSERT_NE(api_host, nullptr);
    ASSERT_TRUE(api_host->strategy);
    ASSERT_TRUE(api_host->strategy->https);
    EXPECT_EQ(*api_host->strategy->https, HttpsStrategy::Redirect301);
    EXPECT_EQ(api_host->strategy->net_mask, kNetVdi | kNetOffice);

    ASSERT_TRUE(config.routes);
    ASSERT_EQ(config.routes->size(), 2U);
    const RouteConfig &proxy = require_route(config, 0);
    ASSERT_TRUE(proxy.path);
    EXPECT_EQ(*proxy.path, "/v1/items/:id");
    ASSERT_TRUE(proxy.type);
    EXPECT_EQ(*proxy.type, RouteType::Proxy);
    ASSERT_TRUE(proxy.service);
    EXPECT_EQ(*proxy.service, "item-service/gray");
    ASSERT_TRUE(proxy.cluster);
    EXPECT_EQ(*proxy.cluster, "stable");
    ASSERT_EQ(proxy.addresses.size(), 1U);
    ASSERT_TRUE(proxy.addresses[0]);
    EXPECT_EQ(*proxy.addresses[0], "127.0.0.1:8080");
    ASSERT_TRUE(proxy.condition);
    EXPECT_EQ(*proxy.condition, "$req.method === 'GET'");
    ASSERT_TRUE(proxy.timeout_millis);
    EXPECT_EQ(*proxy.timeout_millis, 300000);
    ASSERT_TRUE(proxy.max_client_body_size);
    EXPECT_EQ(*proxy.max_client_body_size, 100 * 1024);
    ASSERT_TRUE(proxy.max_proxy_body_size);
    EXPECT_EQ(*proxy.max_proxy_body_size, 10 * 1024 * 1024);
    ASSERT_TRUE(proxy.websocket_timeout_millis);
    EXPECT_EQ(*proxy.websocket_timeout_millis, 300);
    ASSERT_TRUE(proxy.flush);
    EXPECT_TRUE(*proxy.flush);
    ASSERT_EQ(proxy.allows.size(), 2U);

    const StringConfigEntry *optional_header = find_string_entry(proxy.proxy_headers, "X-Optional");
    ASSERT_NE(optional_header, nullptr);
    EXPECT_FALSE(optional_header->value);

    const RouteConfig &response = require_route(config, 1);
    ASSERT_TRUE(response.type);
    EXPECT_EQ(*response.type, RouteType::Response);
    EXPECT_EQ(response.status, 201);
    ASSERT_TRUE(response.body);
    ASSERT_TRUE(response.body->type);
    EXPECT_EQ(*response.body->type, BodyType::Text);
    ASSERT_TRUE(response.body->content);
    EXPECT_EQ(*response.body->content, "ok");
}

TEST(AccessConfigCodecTest, PreservesObservedJacksonCoercionsAndLastWinsFields) {
    const std::string input = read_fixture("project-conf-jackson-coercions.json");
    ASSERT_FALSE(input.empty());

    auto result = parse_project_config(input);

    ASSERT_TRUE(result) << result.error().message;
    ASSERT_TRUE(*result);
    const ProjectConfig &config = **result;
    EXPECT_EQ(config.version, 3);

    const RouteConfig &route = require_route(config, 0);
    ASSERT_TRUE(route.path);
    EXPECT_EQ(*route.path, "7");
    EXPECT_EQ(route.status, 201);
    ASSERT_TRUE(route.flush);
    EXPECT_TRUE(*route.flush);
    ASSERT_TRUE(route.timeout_millis);
    EXPECT_EQ(*route.timeout_millis, 300000);
    ASSERT_TRUE(route.websocket_timeout_millis);
    EXPECT_EQ(*route.websocket_timeout_millis, 250);
    ASSERT_TRUE(route.max_client_body_size);
    EXPECT_EQ(*route.max_client_body_size, -1);
    ASSERT_TRUE(route.max_proxy_body_size);
    EXPECT_EQ(*route.max_proxy_body_size, 10 * 1024 * 1024);
    ASSERT_EQ(route.addresses.size(), 2U);
    EXPECT_TRUE(contains_nullable_string(route.addresses, std::optional<std::string>("127.0.0.1:8080")));
    EXPECT_TRUE(contains_nullable_string(route.addresses, std::nullopt));

    const StringConfigEntry *numeric = find_string_entry(route.response_headers, "X-Numeric");
    ASSERT_NE(numeric, nullptr);
    ASSERT_TRUE(numeric->value);
    EXPECT_EQ(*numeric->value, "6");
    const StringConfigEntry *null_value = find_string_entry(route.response_headers, "X-Null");
    ASSERT_NE(null_value, nullptr);
    EXPECT_FALSE(null_value->value);
}

TEST(AccessConfigCodecTest, KeepsJavaDefaultsAndUnknownEnumAsNull) {
    auto defaults = parse_project_config(R"({"host":{},"routes":[{"path":"/","service":"svc"}]})");
    ASSERT_TRUE(defaults);
    ASSERT_TRUE(*defaults);
    EXPECT_EQ((**defaults).version, 0);
    const RouteConfig &default_route = require_route(**defaults, 0);
    ASSERT_TRUE(default_route.type);
    EXPECT_EQ(*default_route.type, RouteType::Proxy);
    EXPECT_EQ(default_route.status, 0);

    auto unknown = parse_project_config(R"({"host":{},"routes":[{"path":"/","type":"FUTURE","service":"svc"}]})");
    ASSERT_TRUE(unknown);
    ASSERT_TRUE(*unknown);
    EXPECT_FALSE(require_route(**unknown, 0).type);
}

TEST(AccessConfigCodecTest, ParsesJavaDurationAndDataSizeBoundaries) {
    auto result = parse_project_config(R"({"routes":[{"timeout":-1,"websocket_timeout":"2147484s",)"
                                       R"("max_client_body_size":0,"max_proxy_body_size":"1G"}]})");

    ASSERT_TRUE(result) << result.error().message;
    ASSERT_TRUE(*result);
    const RouteConfig &route = require_route(**result, 0);
    ASSERT_TRUE(route.timeout_millis);
    EXPECT_EQ(*route.timeout_millis, -1);
    ASSERT_TRUE(route.websocket_timeout_millis);
    EXPECT_EQ(*route.websocket_timeout_millis, -2147483296LL);
    ASSERT_TRUE(route.max_client_body_size);
    EXPECT_EQ(*route.max_client_body_size, 0);
    ASSERT_TRUE(route.max_proxy_body_size);
    EXPECT_EQ(*route.max_proxy_body_size, 1LL << 30);

    auto string_zero = parse_project_config(R"({"routes":[{"max_client_body_size":"0"}]})");
    ASSERT_FALSE(string_zero);
    EXPECT_EQ(string_zero.error().code, AccessConfigErrorCode::InvalidField);
    EXPECT_EQ(string_zero.error().field, "routes[0].max_client_body_size");
}

TEST(AccessConfigCodecTest, ProcessesDuplicateKnownFieldsInInputOrder) {
    auto valid = parse_project_config(R"({"version":1,"version":"2"})");
    ASSERT_TRUE(valid);
    ASSERT_TRUE(*valid);
    EXPECT_EQ((**valid).version, 2);

    auto invalid = parse_project_config(R"({"version":"not-an-int","version":2})");
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().field, "version");
}

TEST(AccessConfigCodecTest, PreservesJacksonScalarCoercionSpelling) {
    auto result =
            parse_project_config(R"({"routes":[{"path":1.0},{"path":1e3},{"path":1E+3},{"path":-0.0},{"path":true},)"
                                 R"({"status":201.9,"flush":1},{"type":0},{"type":9}]})");

    ASSERT_TRUE(result) << result.error().message;
    ASSERT_TRUE(*result);
    const ProjectConfig &config = **result;
    EXPECT_EQ(*require_route(config, 0).path, "1.0");
    EXPECT_EQ(*require_route(config, 1).path, "1e3");
    EXPECT_EQ(*require_route(config, 2).path, "1E+3");
    EXPECT_EQ(*require_route(config, 3).path, "-0.0");
    EXPECT_EQ(*require_route(config, 4).path, "true");
    EXPECT_EQ(require_route(config, 5).status, 201);
    ASSERT_TRUE(require_route(config, 5).flush);
    EXPECT_TRUE(*require_route(config, 5).flush);
    ASSERT_TRUE(require_route(config, 6).type);
    EXPECT_EQ(*require_route(config, 6).type, RouteType::Proxy);
    EXPECT_FALSE(require_route(config, 7).type);
}

TEST(AccessConfigCodecTest, DistinguishesEmptyContentNullAndInvalidRoot) {
    auto empty = parse_project_config("");
    ASSERT_TRUE(empty);
    EXPECT_FALSE(*empty);

    auto null = parse_project_config("null");
    ASSERT_TRUE(null);
    EXPECT_FALSE(*null);

    auto whitespace = parse_project_config(" ");
    EXPECT_FALSE(whitespace);
    EXPECT_EQ(whitespace.error().code, AccessConfigErrorCode::InvalidJson);

    auto array = parse_project_config("[]");
    EXPECT_FALSE(array);
    EXPECT_EQ(array.error().code, AccessConfigErrorCode::InvalidRoot);
}

TEST(AccessConfigCodecTest, MatchesJavaProjectListSplitSemantics) {
    EXPECT_TRUE(parse_project_list("").empty());
    EXPECT_EQ(parse_project_list(" a;b "), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(parse_project_list("a; b"), (std::vector<std::string>{"a", " b"}));
    EXPECT_EQ(parse_project_list("a;;b;"), (std::vector<std::string>{"a", "", "b"}));
    EXPECT_TRUE(parse_project_list(";").empty());
    EXPECT_EQ(parse_project_list(" "), (std::vector<std::string>{""}));
}

} // namespace
