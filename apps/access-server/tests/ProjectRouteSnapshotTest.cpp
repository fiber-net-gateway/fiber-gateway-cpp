#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>

#include "routing/AccessRouteSnapshot.h"
#include "routing/Cidr.h"
#include "routing/HostMatcher.h"
#include "routing/ProjectRouteSnapshot.h"
#include "runtime/AccessScriptRuntime.h"

namespace {

using fiber::access_server::AccessConfigErrorCode;
using fiber::access_server::AccessRouteSnapshot;
using fiber::access_server::BodyType;
using fiber::access_server::Cidr;
using fiber::access_server::compile_project_config;
using fiber::access_server::CompiledRoute;
using fiber::access_server::HostConfigEntry;
using fiber::access_server::HostMatcher;
using fiber::access_server::HostPattern;
using fiber::access_server::HostStrategyConfig;
using fiber::access_server::HttpsStrategy;
using fiber::access_server::PathVariable;
using fiber::access_server::ProjectConfig;
using fiber::access_server::ProjectRouteSnapshot;
using fiber::access_server::ResponseBodyKind;
using fiber::access_server::RouteBodyConfig;
using fiber::access_server::RouteConfig;
using fiber::access_server::RouteType;
using fiber::access_server::ScriptCompilerAdapter;
using fiber::access_server::StringConfigEntry;

struct ScriptCompilerCapture {
    fiber::access_server::AccessScriptRuntime runtime;
    std::vector<std::string> expressions;
    std::vector<std::vector<std::string>> path_variable_names;
};

ScriptCompilerAdapter::Result capture_expression(void *context, fiber::http_script::ConstPackage::Builder &constants,
                                                 std::string_view expression,
                                                 std::span<const std::string> path_variable_names) {
    auto &capture = *static_cast<ScriptCompilerCapture *>(context);
    capture.expressions.emplace_back(expression);
    capture.path_variable_names.emplace_back(path_variable_names.begin(), path_variable_names.end());
    ScriptCompilerAdapter delegate = capture.runtime.compiler_adapter();
    return delegate.compile_expression(delegate.context, constants, expression, path_variable_names);
}

ScriptCompilerAdapter compiler_adapter(ScriptCompilerCapture &capture) {
    return ScriptCompilerAdapter{
            .context = &capture,
            .compile_expression = capture_expression,
    };
}

HostConfigEntry host(std::string pattern, std::uint8_t net_mask = 0) {
    return HostConfigEntry{
            .pattern = std::move(pattern),
            .strategy =
                    HostStrategyConfig{
                            .https = HttpsStrategy::NotRequired,
                            .net_mask = net_mask,
                    },
    };
}

RouteConfig proxy_route(std::string path, std::string service = "service") {
    RouteConfig route;
    route.path = std::move(path);
    route.service = std::move(service);
    return route;
}

RouteConfig response_route(std::string path, std::int32_t status = 200) {
    RouteConfig route;
    route.path = std::move(path);
    route.type = RouteType::Response;
    route.status = status;
    return route;
}

ProjectConfig project_with_routes(std::vector<std::optional<RouteConfig>> routes) {
    ProjectConfig config;
    config.version = 7;
    config.hosts = std::vector<HostConfigEntry>{host("api.example.com")};
    config.routes = std::move(routes);
    return config;
}

const ProjectRouteSnapshot &require_snapshot(const fiber::access_server::ProjectSnapshotResult &result) {
    EXPECT_TRUE(result) << (result ? "" : result.error().message);
    EXPECT_TRUE(result && *result);
    return **result;
}

class TestRouteMatchContext {
public:
    TestRouteMatchContext(const std::vector<CompiledRoute> &routes, std::span<PathVariable> path_variables,
                          std::uint32_t accepted_conditional_route = std::numeric_limits<std::uint32_t>::max()) noexcept
        : routes_(routes), path_variables_(path_variables), accepted_conditional_route_(accepted_conditional_route) {}

    bool matched(std::uint32_t, std::uint32_t route_index) noexcept {
        const CompiledRoute &route = routes_[route_index];
        if (route.condition_program && route_index != accepted_conditional_route_) {
            return false;
        }
        matched_route_ = &route;
        matched_path_variables_ = path_variables_.first(path_variable_count_);
        return true;
    }

    void add_path_var(std::string_view name, std::string_view value) noexcept {
        path_variables_[path_variable_count_++] = PathVariable{
                .name = name,
                .value = value,
        };
    }

    void pop_path_var() noexcept { --path_variable_count_; }

    [[nodiscard]] const CompiledRoute *route() const noexcept { return matched_route_; }
    [[nodiscard]] std::span<const PathVariable> path_variables() const noexcept { return matched_path_variables_; }

private:
    const std::vector<CompiledRoute> &routes_;
    std::span<PathVariable> path_variables_;
    std::uint32_t accepted_conditional_route_;
    const CompiledRoute *matched_route_ = nullptr;
    std::span<const PathVariable> matched_path_variables_;
    std::size_t path_variable_count_ = 0;
};

std::vector<fiber::access_server::AccessUpstreamInstance>
select_addresses(fiber::access_server::ProxyAddressSelector &selector, std::size_t count) {
    fiber::event::EventLoop loop;
    std::vector<fiber::access_server::AccessUpstreamInstance> addresses;
    std::vector<std::uint64_t> excluded;
    bool completed = false;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        for (std::size_t i = 0; i < count; ++i) {
            auto selected = selector.select_address(std::nullopt, excluded);
            EXPECT_TRUE(selected);
            if (!selected || selected->connection_key == nullptr) {
                break;
            }
            addresses.push_back(fiber::access_server::AccessUpstreamInstance{
                    .connection_key = *selected->connection_key,
                    .authority = std::string(selected->host_header),
            });
            excluded.push_back(selected->selection_token);
            selected->report(true);
        }
        completed = true;
        loop.stop();
        co_return;
    });
    loop.run();
    EXPECT_TRUE(completed);
    return addresses;
}

TEST(HostMatcherTest, MatchesJavaNormalizationAndWildcardRules) {
    const std::vector<HostPattern> patterns{
            {.pattern = "*.example.com", .handler = 1},
            {.pattern = "api.example.com", .handler = 2},
            {.pattern = "[::1]", .handler = 3},
    };
    auto built = HostMatcher::build(patterns);
    ASSERT_TRUE(built) << built.error().message;

    EXPECT_EQ(built->match("API.EXAMPLE.COM"), 2);
    EXPECT_EQ(built->match("api.example.com:443"), 2);
    EXPECT_EQ(built->match("api.example.com.:443"), 2);
    EXPECT_EQ(built->match(".api.example.com"), 2);
    EXPECT_EQ(built->match("a.example.com"), 1);
    EXPECT_EQ(built->match("a.b.example.com"), 1);
    EXPECT_FALSE(built->match("example.com"));
    EXPECT_EQ(built->match("[::1]:8080"), 3);

    EXPECT_FALSE(built->match(""));
    EXPECT_FALSE(built->match("."));
    EXPECT_FALSE(built->match("a..example.com"));
    EXPECT_FALSE(built->match("api.example.com/path"));
    EXPECT_FALSE(built->match("api.example.com\n"));
}

TEST(HostMatcherTest, DoesNotFallbackFromAnExistingExactBranch) {
    auto suffix = HostMatcher::build(std::array{
            HostPattern{.pattern = "*.example.com", .handler = 1},
            HostPattern{.pattern = "api.example.com", .handler = 2},
    });
    ASSERT_TRUE(suffix);
    EXPECT_FALSE(suffix->match("x.api.example.com"));
    EXPECT_EQ(suffix->match("x.example.com"), 1);

    auto global = HostMatcher::build(std::array{
            HostPattern{.pattern = "*", .handler = 3},
            HostPattern{.pattern = "example.com", .handler = 4},
    });
    ASSERT_TRUE(global);
    EXPECT_FALSE(global->match("x.example.com"));
    EXPECT_EQ(global->match("unrelated.test"), 3);
}

TEST(HostMatcherTest, RejectsDuplicateAndUnsupportedWildcardPatterns) {
    auto duplicate = HostMatcher::build(std::array{
            HostPattern{.pattern = "API.example.com", .handler = 1},
            HostPattern{.pattern = "api.EXAMPLE.com", .handler = 2},
    });
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, AccessConfigErrorCode::InvalidField);

    auto folded_punctuation = HostMatcher::build(std::array{
            HostPattern{.pattern = "@.example.com", .handler = 1},
            HostPattern{.pattern = "`.example.com", .handler = 2},
    });
    EXPECT_FALSE(folded_punctuation);

    auto malformed = HostMatcher::build(std::array{HostPattern{.pattern = "api.*.com", .handler = 1}});
    EXPECT_FALSE(malformed);
}

TEST(CidrTest, ParsesMatchesAndRemovesContainedNetworks) {
    const std::array<std::string_view, 3> values{"192.168.34.4/32", "192.168.0.1/16", "2001:db8::/32"};
    auto parsed = Cidr::parse_list(values, "allows");
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed->size(), 2U);

    fiber::net::IpAddress inside_v4{};
    ASSERT_TRUE(fiber::net::IpAddress::parse("192.168.99.1", inside_v4));
    fiber::net::IpAddress outside_v4{};
    ASSERT_TRUE(fiber::net::IpAddress::parse("192.169.0.1", outside_v4));
    fiber::net::IpAddress inside_v6{};
    ASSERT_TRUE(fiber::net::IpAddress::parse("2001:db8::123", inside_v6));

    EXPECT_TRUE((*parsed)[0].matches(inside_v4));
    EXPECT_FALSE((*parsed)[0].matches(outside_v4));
    EXPECT_TRUE((*parsed)[1].matches(inside_v6));

    auto narrow_network = Cidr::parse("192.0.0.0/16", "allows");
    ASSERT_TRUE(narrow_network);
    auto broad_target = Cidr::parse("192.0.0.0/8", "target");
    ASSERT_TRUE(broad_target);
    EXPECT_TRUE(narrow_network->matches(*broad_target));
    EXPECT_FALSE(narrow_network->contains(*broad_target));

    EXPECT_FALSE(Cidr::parse("192.168.1.1/33", "allows"));
    EXPECT_FALSE(Cidr::parse("not-an-ip", "allows"));
    EXPECT_TRUE(Cidr::parse(".1.2.3/8", "allows"));
    EXPECT_FALSE(Cidr::parse("01.2.3.4", "allows"));
    EXPECT_TRUE(Cidr::parse("[::1x/128", "allows"));
}

TEST(ProjectRouteSnapshotTest, TreatsEmptyHostMapAsUnloadBeforeRouteCompilation) {
    ProjectConfig config;
    config.version = 9;
    config.routes = std::nullopt;

    auto missing = compile_project_config("demo", config);
    ASSERT_TRUE(missing);
    EXPECT_FALSE(*missing);

    config.hosts = std::vector<HostConfigEntry>{};
    auto empty = compile_project_config("demo", config);
    ASSERT_TRUE(empty);
    EXPECT_FALSE(*empty);

    auto empty_project = compile_project_config("", config);
    EXPECT_FALSE(empty_project);
}

TEST(ProjectRouteSnapshotTest, RequiresRoutesOnlyForConfiguredHosts) {
    ProjectConfig config;
    config.hosts = std::vector<HostConfigEntry>{host("api.example.com")};

    auto result = compile_project_config("demo", config);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().field, "routes");
}

TEST(ProjectRouteSnapshotTest, CompilesProxyFieldsAndJavaNarrowing) {
    fiber::access_server::AccessScriptRuntime scripts;
    RouteConfig route = proxy_route("/v1/:id", "orders/gray");
    route.cluster = "stable";
    route.timeout_millis = 4'294'967'297LL;
    route.websocket_timeout_millis = 4'294'967'298LL;
    route.max_client_body_size = -1;
    route.max_proxy_body_size = 1024;
    route.flush = true;
    route.proxy_headers.push_back(StringConfigEntry{.name = "X-Id", .value = "${$path.id}"});
    route.response_headers.push_back(StringConfigEntry{.name = "X-Result", .value = "ok"});
    route.context.push_back(StringConfigEntry{.name = "cluster", .value = "blue"});
    route.rewrite = R"(/items/${$path.id})";
    route.allows = {
            std::optional<std::string>("192.168.1.1/24"),
            std::optional<std::string>("192.168.1.2/32"),
            std::optional<std::string>("!10.0.0.0/8"),
    };

    auto result = compile_project_config("orders", project_with_routes({std::move(route)}), scripts.compiler_adapter());
    const ProjectRouteSnapshot &snapshot = require_snapshot(result);
    ASSERT_EQ(snapshot.routes().size(), 1U);
    const CompiledRoute &compiled = snapshot.routes()[0];
    ASSERT_TRUE(compiled.proxy);
    ASSERT_TRUE(compiled.proxy->address_selector);
    EXPECT_EQ(compiled.proxy->address_selector->service_name(), "orders");
    ASSERT_TRUE(compiled.proxy->address_selector->configured_cluster());
    EXPECT_EQ(*compiled.proxy->address_selector->configured_cluster(), "stable");
    EXPECT_EQ(compiled.proxy->timeout_millis, 1);
    ASSERT_TRUE(compiled.proxy->websocket_timeout_millis);
    EXPECT_EQ(*compiled.proxy->websocket_timeout_millis, 2);
    EXPECT_EQ(compiled.proxy->max_response_body_size, 1024);
    EXPECT_EQ(compiled.max_client_body_size, -1);
    EXPECT_EQ(compiled.allow_cidrs.size(), 1U);
    EXPECT_EQ(compiled.deny_cidrs.size(), 1U);
    EXPECT_EQ(compiled.proxy->proxy_headers.size(), 1U);
    EXPECT_EQ(compiled.proxy->response_headers.size(), 1U);
    EXPECT_EQ(compiled.proxy->context.size(), 1U);
    EXPECT_EQ(compiled.proxy->context[0].name, "HI-TRACE-CLUSTER");
    ASSERT_TRUE(compiled.proxy->rewrite);
    ASSERT_EQ(compiled.proxy->rewrite->expressions.size(), 1U);
    EXPECT_EQ(compiled.proxy->rewrite->expressions[0].leading_literal, "/items/");
    EXPECT_EQ(compiled.proxy->rewrite->expressions[0].source, "$path.id");
    EXPECT_TRUE(compiled.proxy->rewrite->expressions[0].program.valid());
    EXPECT_TRUE(compiled.proxy->rewrite->trailing_literal.empty());
}

TEST(ProjectRouteSnapshotTest, ResolvesConcreteServiceClusterWhileCompilingRoutes) {
    RouteConfig default_cluster = proxy_route("/default", "orders");
    RouteConfig service_cluster = proxy_route("/service-cluster", "orders/gray");
    RouteConfig empty_service_cluster = proxy_route("/empty-service-cluster", "orders/");
    RouteConfig explicit_cluster = proxy_route("/explicit-cluster", "orders/gray");
    explicit_cluster.cluster = "stable";

    auto result = compile_project_config(
            "orders", project_with_routes({std::move(default_cluster), std::move(service_cluster),
                                           std::move(empty_service_cluster), std::move(explicit_cluster)}));
    const ProjectRouteSnapshot &snapshot = require_snapshot(result);
    ASSERT_EQ(snapshot.routes().size(), 4U);

    const std::array<std::string_view, 4> expected_clusters = {"default", "gray", "default", "stable"};
    for (std::size_t i = 0; i < expected_clusters.size(); ++i) {
        ASSERT_TRUE(snapshot.routes()[i].proxy);
        ASSERT_TRUE(snapshot.routes()[i].proxy->address_selector);
        const auto cluster = snapshot.routes()[i].proxy->address_selector->configured_cluster();
        ASSERT_TRUE(cluster);
        EXPECT_EQ(*cluster, expected_clusters[i]);
    }
}

TEST(ProjectRouteSnapshotTest, CompilesStaticAddressesWithJavaHttpHostRules) {
    RouteConfig route = proxy_route("/address", "");
    route.addresses = {
            std::optional<std::string>("http://127.0.0.1:8080"),
            std::optional<std::string>("backend:-2147483648"),
            std::optional<std::string>(":80"),
            std::optional<std::string>("backend:443"),
    };

    auto result = compile_project_config("demo", project_with_routes({std::move(route)}));
    const ProjectRouteSnapshot &snapshot = require_snapshot(result);
    ASSERT_TRUE(snapshot.routes()[0].proxy);
    ASSERT_TRUE(snapshot.routes()[0].proxy->address_selector);
    EXPECT_TRUE(snapshot.routes()[0].proxy->address_selector->service_name().empty());
    const auto addresses = select_addresses(*snapshot.routes()[0].proxy->address_selector, 4);
    ASSERT_EQ(addresses.size(), 4U);
    using ConnectionKey = fiber::http::Http1ConnectionGroupKey;
    EXPECT_EQ(addresses[0].connection_key.scheme(), ConnectionKey::Scheme::Http);
    EXPECT_TRUE(addresses[0].connection_key.is_ip());
    EXPECT_EQ(addresses[0].connection_key.ip_address().to_string(), "127.0.0.1");
    EXPECT_EQ(addresses[0].connection_key.port(), 8080);
    EXPECT_EQ(addresses[0].authority, "127.0.0.1:8080");
    EXPECT_EQ(addresses[1].connection_key.scheme(), ConnectionKey::Scheme::Http);
    EXPECT_TRUE(addresses[1].connection_key.is_name());
    EXPECT_EQ(addresses[1].connection_key.host_name(), "backend");
    EXPECT_EQ(addresses[1].connection_key.port(), 80);
    EXPECT_EQ(addresses[1].authority, "backend");
    EXPECT_EQ(addresses[2].connection_key.host_name(), ":80");
    EXPECT_EQ(addresses[2].authority, ":80");
    EXPECT_EQ(addresses[3].connection_key.scheme(), ConnectionKey::Scheme::Https);
    EXPECT_EQ(addresses[3].connection_key.port(), 443);
    EXPECT_EQ(addresses[3].authority, "backend");

    RouteConfig invalid = proxy_route("/address", "");
    invalid.addresses = {std::optional<std::string>("backend:2147483648")};
    auto rejected = compile_project_config("demo", project_with_routes({std::move(invalid)}));
    EXPECT_FALSE(rejected);

    RouteConfig oversized = proxy_route("/address", "");
    oversized.addresses = {std::optional<std::string>("backend:70000")};
    auto oversized_rejected = compile_project_config("demo", project_with_routes({std::move(oversized)}));
    EXPECT_FALSE(oversized_rejected);
}

TEST(ProjectRouteSnapshotTest, CompilesResponseBodyAndHeaders) {
    RouteConfig route = response_route("/created", 201);
    route.body = RouteBodyConfig{
            .type = BodyType::Base64,
            .content = "aGVsbG8=",
    };
    route.response_headers.push_back(StringConfigEntry{.name = "Content-Type", .value = "text/plain"});

    auto result = compile_project_config("demo", project_with_routes({std::move(route)}));
    const ProjectRouteSnapshot &snapshot = require_snapshot(result);
    ASSERT_TRUE(snapshot.routes()[0].response);
    EXPECT_EQ(snapshot.routes()[0].response->status, 201);
    EXPECT_EQ(snapshot.routes()[0].response->body_kind, ResponseBodyKind::Base64);
    EXPECT_EQ(snapshot.routes()[0].response->body, "hello");
    EXPECT_EQ(snapshot.routes()[0].response->response_headers.size(), 1U);
}

TEST(ProjectRouteSnapshotTest, BindsPreparsedTemplateExpressionsAfterDiscoveringPathVariables) {
    RouteConfig route = response_route("/items/:id", 200);
    route.body = RouteBodyConfig{
            .type = BodyType::Template,
            .content = "item-${$path.id}-${$req.method}",
    };
    route.response_headers.push_back(StringConfigEntry{.name = "X-Item", .value = "${$path.id}"});
    route.response_headers.push_back(StringConfigEntry{.name = "X-Static", .value = "static"});
    ScriptCompilerCapture capture;

    auto result = compile_project_config("demo", project_with_routes({std::move(route)}), compiler_adapter(capture));
    const ProjectRouteSnapshot &snapshot = require_snapshot(result);
    ASSERT_TRUE(snapshot.routes()[0].response);
    const auto &response = *snapshot.routes()[0].response;
    ASSERT_TRUE(response.body_template);
    ASSERT_EQ(response.body_template->expressions.size(), 2U);
    EXPECT_TRUE(response.body_template->expressions[0].program.valid());
    EXPECT_TRUE(response.body_template->expressions[1].program.valid());
    ASSERT_EQ(response.response_headers.size(), 2U);
    ASSERT_EQ(response.response_headers[0].value.expressions.size(), 1U);
    EXPECT_TRUE(response.response_headers[0].value.expressions[0].program.valid());
    EXPECT_FALSE(response.response_headers[1].value.dynamic());

    EXPECT_EQ(capture.expressions, (std::vector<std::string>{"$path.id", "$req.method", "$path.id"}));
    ASSERT_EQ(capture.path_variable_names.size(), 3U);
    for (const auto &names: capture.path_variable_names) {
        EXPECT_EQ(names, (std::vector<std::string>{"id"}));
    }
}

TEST(ProjectRouteSnapshotTest, BindsProxyHeaderTemplatesBeforeFreezingThem) {
    RouteConfig route = proxy_route("/items/:id");
    route.proxy_headers.push_back(StringConfigEntry{.name = "X-Item", .value = "${$path.id}"});
    route.response_headers.push_back(StringConfigEntry{.name = "X-Reply", .value = "${$path.id}"});
    ScriptCompilerCapture capture;

    auto result = compile_project_config("demo", project_with_routes({std::move(route)}), compiler_adapter(capture));
    const ProjectRouteSnapshot &snapshot = require_snapshot(result);
    ASSERT_TRUE(snapshot.routes()[0].proxy);
    const auto &proxy = *snapshot.routes()[0].proxy;

    ASSERT_EQ(proxy.proxy_headers.size(), 1U);
    const auto proxy_header = *proxy.proxy_headers.begin();
    EXPECT_EQ(proxy_header.name(), "X-Item");
    ASSERT_EQ(proxy_header.value().expressions.size(), 1U);
    EXPECT_TRUE(proxy_header.value().expressions[0].program.valid());

    ASSERT_EQ(proxy.response_headers.size(), 1U);
    const auto response_header = *proxy.response_headers.begin();
    EXPECT_EQ(response_header.name(), "X-Reply");
    ASSERT_EQ(response_header.value().expressions.size(), 1U);
    EXPECT_TRUE(response_header.value().expressions[0].program.valid());

    EXPECT_EQ(capture.expressions, (std::vector<std::string>{"$path.id", "$path.id"}));
    ASSERT_EQ(capture.path_variable_names.size(), 2U);
    EXPECT_EQ(capture.path_variable_names[0], (std::vector<std::string>{"id"}));
    EXPECT_EQ(capture.path_variable_names[1], (std::vector<std::string>{"id"}));
}

TEST(ProjectRouteSnapshotTest, RejectsCaseInsensitiveProxyHeaderDuplicates) {
    RouteConfig route = proxy_route("/items");
    route.proxy_headers.push_back(StringConfigEntry{.name = "X-Duplicate", .value = "first"});
    route.proxy_headers.push_back(StringConfigEntry{.name = "x-duplicate", .value = "second"});

    auto result = compile_project_config("demo", project_with_routes({std::move(route)}));

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, AccessConfigErrorCode::Conflict);
    EXPECT_EQ(result.error().field, "routes[0].proxy_headers");
    EXPECT_EQ(result.error().message, "header name is duplicate ignoring ASCII case");
}

TEST(ProjectRouteSnapshotTest, UsesJavaCrc32cRouteKeyAndConditionalOrder) {
    fiber::access_server::AccessScriptRuntime scripts;
    RouteConfig first = proxy_route("/same/:id");
    first.condition = "false";
    RouteConfig second = proxy_route("/same/:id");
    second.condition = "123456789";
    RouteConfig fallback = proxy_route("/same/:id");

    auto result = compile_project_config(
            "demo", project_with_routes({std::move(first), std::move(second), std::move(fallback)}),
            scripts.compiler_adapter());
    const ProjectRouteSnapshot &snapshot = require_snapshot(result);
    ASSERT_EQ(snapshot.routes().size(), 3U);
    ASSERT_TRUE(snapshot.routes()[0].condition_program);
    EXPECT_TRUE(snapshot.routes()[0].condition_program->valid());
    ASSERT_TRUE(snapshot.routes()[1].condition_program);
    EXPECT_TRUE(snapshot.routes()[1].condition_program->valid());
    EXPECT_FALSE(snapshot.routes()[2].condition_program);
    EXPECT_EQ(snapshot.routes()[1].key, "/same/:id@3829603e");

    std::array<PathVariable, 1> variables;
    TestRouteMatchContext context(snapshot.routes(), variables, 1);
    ASSERT_TRUE(snapshot.match_route_path("/same/42", context));
    EXPECT_EQ(context.route(), &snapshot.routes()[1]);
    ASSERT_EQ(context.path_variables().size(), 1U);
    EXPECT_EQ(context.path_variables()[0].name, "id");
    EXPECT_EQ(context.path_variables()[0].value, "42");

    TestRouteMatchContext no_script_context(snapshot.routes(), variables);
    ASSERT_TRUE(snapshot.match_route_path("/same/43", no_script_context));
    EXPECT_EQ(no_script_context.route(), &snapshot.routes()[2]);
}

TEST(ProjectRouteSnapshotTest, RejectsRouteAfterUnconditionalAtTheSameNode) {
    RouteConfig unconditional = proxy_route("/same/:id");
    RouteConfig dead = proxy_route("/same/:id");
    dead.condition = "true";

    auto result = compile_project_config("demo", project_with_routes({std::move(unconditional), std::move(dead)}));

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, AccessConfigErrorCode::Conflict);
    EXPECT_EQ(result.error().field, "routes[1].path");
}

TEST(ProjectRouteSnapshotTest, RejectsDuplicatePathVariableAndMiddleWildcard) {
    auto duplicate = compile_project_config("demo", project_with_routes({proxy_route("/items/:id/:id")}));
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, AccessConfigErrorCode::Conflict);

    auto wildcard = compile_project_config("demo", project_with_routes({proxy_route("/items/*rest/more")}));
    ASSERT_FALSE(wildcard);
    EXPECT_EQ(wildcard.error().field, "routes[0].path");
}

TEST(ProjectRouteSnapshotTest, ReportsMaximumPathVariableCount) {
    auto result = compile_project_config("demo", project_with_routes({proxy_route("/:a/:b")}));
    const ProjectRouteSnapshot &snapshot = require_snapshot(result);

    EXPECT_EQ(snapshot.max_path_variable_count(), 2U);
    std::array<PathVariable, 2> variables;
    TestRouteMatchContext context(snapshot.routes(), variables);
    ASSERT_TRUE(snapshot.match_route_path("/x/y", context));
    ASSERT_EQ(context.path_variables().size(), 2U);
    EXPECT_EQ(context.path_variables()[0].value, "x");
    EXPECT_EQ(context.path_variables()[1].value, "y");
}

TEST(ProjectRouteSnapshotTest, RejectsJavaBuildTimeInvalidCombinations) {
    {
        RouteConfig route = response_route("/bad", 99);
        auto result = compile_project_config("demo", project_with_routes({std::move(route)}));
        EXPECT_FALSE(result);
    }
    {
        RouteConfig route = response_route("/bad");
        route.body = RouteBodyConfig{.type = BodyType::Text, .content = ""};
        auto result = compile_project_config("demo", project_with_routes({std::move(route)}));
        EXPECT_FALSE(result);
    }
    {
        RouteConfig route = response_route("/bad");
        route.body = RouteBodyConfig{.type = BodyType::Base64, .content = "not base64"};
        auto result = compile_project_config("demo", project_with_routes({std::move(route)}));
        EXPECT_FALSE(result);
    }
    {
        RouteConfig route = proxy_route("/bad");
        route.timeout_millis = 4;
        auto result = compile_project_config("demo", project_with_routes({std::move(route)}));
        EXPECT_FALSE(result);
    }
    {
        RouteConfig route = proxy_route("/bad", "");
        auto result = compile_project_config("demo", project_with_routes({std::move(route)}));
        EXPECT_FALSE(result);
    }
    {
        RouteConfig route = proxy_route("/bad");
        route.rewrite = R"(${unclosed)";
        auto result = compile_project_config("demo", project_with_routes({std::move(route)}));
        EXPECT_FALSE(result);
    }
    {
        ProjectConfig config = project_with_routes({proxy_route("/ok")});
        (*config.hosts)[0].strategy = std::nullopt;
        auto result = compile_project_config("demo", config);
        EXPECT_FALSE(result);
    }
}

TEST(ProjectRouteSnapshotTest, KeepsConfiguredButEmptyHostPatternsUnreachable) {
    ProjectConfig config = project_with_routes({proxy_route("/ok")});
    config.hosts = std::vector<HostConfigEntry>{
            HostConfigEntry{.pattern = "", .strategy = std::nullopt},
    };

    auto result = compile_project_config("demo", config);
    const ProjectRouteSnapshot &snapshot = require_snapshot(result);
    EXPECT_TRUE(snapshot.hosts().empty());
    EXPECT_EQ(snapshot.match_host("api.example.com"), nullptr);
}

TEST(AccessRouteSnapshotTest, SelectsProjectsThroughOneGlobalHostTree) {
    ProjectConfig wildcard_config = project_with_routes({proxy_route("/wild")});
    wildcard_config.hosts = std::vector<HostConfigEntry>{host("*.example.com", 1)};
    auto wildcard_result = compile_project_config("wildcard", wildcard_config);
    ASSERT_TRUE(wildcard_result);
    ASSERT_TRUE(*wildcard_result);

    ProjectConfig exact_config = project_with_routes({proxy_route("/exact")});
    exact_config.hosts = std::vector<HostConfigEntry>{host("api.example.com", 2)};
    auto exact_result = compile_project_config("exact", exact_config);
    ASSERT_TRUE(exact_result);
    ASSERT_TRUE(*exact_result);

    const std::vector<std::shared_ptr<const ProjectRouteSnapshot>> projects{
            std::make_shared<ProjectRouteSnapshot>(std::move(**wildcard_result)),
            std::make_shared<ProjectRouteSnapshot>(std::move(**exact_result)),
    };
    auto snapshot = AccessRouteSnapshot::build(projects);
    ASSERT_TRUE(snapshot) << snapshot.error().message;

    auto exact = snapshot->match_host("API.EXAMPLE.COM:443");
    ASSERT_TRUE(exact);
    EXPECT_EQ(exact.project->project(), "exact");
    EXPECT_EQ(exact.host->strategy.net_mask, 2);

    auto wildcard = snapshot->match_host("a.b.example.com");
    ASSERT_TRUE(wildcard);
    EXPECT_EQ(wildcard.project->project(), "wildcard");
    EXPECT_EQ(wildcard.host->strategy.net_mask, 1);

    EXPECT_FALSE(snapshot->match_host("example.com"));
}

TEST(AccessRouteSnapshotTest, RejectsCrossProjectDuplicateHosts) {
    ProjectConfig left_config = project_with_routes({proxy_route("/left")});
    left_config.hosts = std::vector<HostConfigEntry>{host("API.example.com")};
    auto left_result = compile_project_config("left", left_config);
    ASSERT_TRUE(left_result);
    ASSERT_TRUE(*left_result);

    ProjectConfig right_config = project_with_routes({proxy_route("/right")});
    right_config.hosts = std::vector<HostConfigEntry>{host("api.EXAMPLE.com")};
    auto right_result = compile_project_config("right", right_config);
    ASSERT_TRUE(right_result);
    ASSERT_TRUE(*right_result);

    const std::vector<std::shared_ptr<const ProjectRouteSnapshot>> projects{
            std::make_shared<ProjectRouteSnapshot>(std::move(**left_result)),
            std::make_shared<ProjectRouteSnapshot>(std::move(**right_result)),
    };
    auto snapshot = AccessRouteSnapshot::build(projects);
    ASSERT_FALSE(snapshot);
    EXPECT_EQ(snapshot.error().code, AccessConfigErrorCode::InvalidField);
}

} // namespace
