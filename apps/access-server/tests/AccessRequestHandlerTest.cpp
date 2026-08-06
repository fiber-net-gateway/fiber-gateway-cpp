#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../tests/HttpTransportStub.h"
#include "async/Spawn.h"
#include "event/EventLoopGroup.h"
#include "execution/AccessRequestHandler.h"
#include "http/Http1Connection.h"
#include "http/HttpBodySpec.h"
#include "http/HttpHeaderHash.h"
#include "runtime/AccessScriptRuntime.h"

namespace {

using namespace std::chrono_literals;

using fiber::access_server::AccessError;
using fiber::access_server::AccessProxyAdapter;
using fiber::access_server::AccessRequestHandler;
using fiber::access_server::AccessRequestHandlerOptions;
using fiber::access_server::AccessRequestScriptAdapter;
using fiber::access_server::AccessScriptRuntime;
using fiber::access_server::BodyType;
using fiber::access_server::HostConfigEntry;
using fiber::access_server::HostStrategyConfig;
using fiber::access_server::HttpsStrategy;
using fiber::access_server::PathVariable;
using fiber::access_server::ProjectConfig;
using fiber::access_server::ProxyExecutionInput;
using fiber::access_server::RouteBodyConfig;
using fiber::access_server::RouteConfig;
using fiber::access_server::RouteConfigStore;
using fiber::access_server::RouteType;
using fiber::access_server::StringConfigEntry;

class RecordingTransport final : public fiber::test::HttpTransportStub {
public:
    RecordingTransport(fiber::event::EventLoop &loop, std::string input, std::string &output) :
        loop_(loop), input_(std::move(input)), output_(output) {}

    fiber::async::Task<fiber::common::IoResult<void>> handshake(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> shutdown(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> wait_readable(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> read(void *, std::size_t,
                                                                  std::chrono::milliseconds) override {
        co_return static_cast<std::size_t>(0);
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> read_into(fiber::mem::IoBuf &buffer,
                                                                       std::chrono::milliseconds) override {
        if (input_consumed_) {
            co_return static_cast<std::size_t>(0);
        }
        if (buffer.writable() < input_.size()) {
            co_return std::unexpected(fiber::common::IoErr::MessageTooLarge);
        }
        std::memcpy(buffer.writable_data(), input_.data(), input_.size());
        buffer.commit(input_.size());
        input_consumed_ = true;
        co_return input_.size();
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> readv_into(fiber::mem::IoBufChain &,
                                                                        std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> write(const void *buffer, std::size_t size,
                                                                   std::chrono::milliseconds) override {
        output_.append(static_cast<const char *>(buffer), size);
        co_return size;
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> write(fiber::mem::IoBuf &buffer,
                                                                   std::chrono::milliseconds) override {
        const std::size_t size = buffer.readable();
        output_.append(reinterpret_cast<const char *>(buffer.readable_data()), size);
        buffer.consume(size);
        co_return size;
    }

    fiber::async::Task<fiber::common::IoResult<std::size_t>> writev(fiber::mem::IoBufChain &buffers,
                                                                    std::chrono::milliseconds) override {
        std::array<iovec, 16> iov{};
        const int count = buffers.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
        std::size_t size = 0;
        for (int i = 0; i < count; ++i) {
            const auto &entry = iov[static_cast<std::size_t>(i)];
            output_.append(static_cast<const char *>(entry.iov_base), entry.iov_len);
            size += entry.iov_len;
        }
        buffers.consume_and_compact(size);
        co_return size;
    }

    void close() override { closed_ = true; }
    [[nodiscard]] bool valid() const noexcept override { return !closed_; }
    [[nodiscard]] int fd() const noexcept override { return -1; }
    [[nodiscard]] std::string_view negotiated_alpn() const noexcept override { return {}; }
    [[nodiscard]] const fiber::net::SocketAddress &remote_addr() const noexcept override { return remote_addr_; }
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept override { return loop_; }

private:
    fiber::event::EventLoop &loop_;
    std::string input_;
    std::string &output_;
    fiber::net::SocketAddress remote_addr_{};
    bool input_consumed_ = false;
    bool closed_ = false;
};

fiber::async::DetachedTask run_request_on_loop(fiber::event::EventLoop *loop, const RouteConfigStore *store,
                                               AccessRequestScriptAdapter script_adapter,
                                               AccessRequestHandlerOptions options, AccessProxyAdapter proxy_adapter,
                                               std::string request, std::string *output, std::promise<void> *done) {
    auto transport = std::make_unique<RecordingTransport>(*loop, std::move(request), *output);
    AccessRequestHandler access_handler(*store, script_adapter, options, proxy_adapter);
    fiber::http::HttpHandler handler = [&access_handler](fiber::http::HttpExchange &exchange) {
        return access_handler.handle(exchange);
    };
    fiber::http::Http1Connection connection(nullptr, std::move(transport), std::move(handler), {});
    co_await connection.run();
    done->set_value();
    co_return;
}

fiber::async::DetachedTask run_committed_response_on_loop(fiber::event::EventLoop *loop, std::string *output,
                                                          std::promise<void> *done) {
    auto transport = std::make_unique<RecordingTransport>(*loop,
                                                          "GET /committed HTTP/1.1\r\n"
                                                          "Host: api.example.com\r\n"
                                                          "Connection: close\r\n\r\n",
                                                          *output);
    fiber::http::HttpHandler handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
        auto sent = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = 204,
                .body = fiber::http::HttpBodySpec::ContentLength(0),
                .end_stream = true,
        });
        if (sent) {
            fiber::access_server::ErrorResponder responder;
            (void) co_await responder.send(exchange, AccessError::entry_error(), {}, {}, "trace", true);
        }
        co_return;
    };
    fiber::http::Http1Connection connection(nullptr, std::move(transport), std::move(handler), {});
    co_await connection.run();
    done->set_value();
    co_return;
}

std::string run_request(const RouteConfigStore &store, std::string request,
                        AccessRequestScriptAdapter script_adapter = {}, AccessRequestHandlerOptions options = {},
                        AccessProxyAdapter proxy_adapter = {}) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::string output;
    std::promise<void> done;
    auto completed = done.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_request_on_loop(&group.at(0), &store, script_adapter, options, proxy_adapter, std::move(request),
                                   &output, &done);
    });

    EXPECT_EQ(completed.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    return output;
}

std::string run_committed_response() {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::string output;
    std::promise<void> done;
    auto completed = done.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_committed_response_on_loop(&group.at(0), &output, &done); });

    EXPECT_EQ(completed.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    return output;
}

std::string_view response_body(std::string_view response) {
    const std::size_t separator = response.find("\r\n\r\n");
    if (separator == std::string_view::npos) {
        return {};
    }
    return response.substr(separator + 4);
}

HostConfigEntry host(std::string pattern, HostStrategyConfig strategy = {}) {
    return HostConfigEntry{
            .pattern = std::move(pattern),
            .strategy = strategy,
    };
}

RouteConfig response_route(std::string path, std::string body, std::int32_t status = 200) {
    RouteConfig route;
    route.path = std::move(path);
    route.type = RouteType::Response;
    route.status = status;
    route.body = RouteBodyConfig{
            .type = BodyType::Template,
            .content = std::move(body),
    };
    return route;
}

RouteConfig proxy_route(std::string path, std::string service = "orders/gray") {
    RouteConfig route;
    route.path = std::move(path);
    route.service = std::move(service);
    return route;
}

ProjectConfig project(HostStrategyConfig strategy, std::vector<std::optional<RouteConfig>> routes) {
    ProjectConfig config;
    config.version = 1;
    config.hosts = std::vector<HostConfigEntry>{host("api.example.com", strategy)};
    config.routes = std::move(routes);
    return config;
}

void publish(RouteConfigStore &store, ProjectConfig config) {
    auto result = store.apply("orders", config);
    ASSERT_TRUE(result) << result.error().message;
}

bool evaluate_condition(void *, fiber::http::HttpExchange &, std::span<const PathVariable> path_variables,
                        std::string_view, const void *, std::string_view expression) noexcept {
    return expression == "id-is-42" && path_variables.size() == 1 && path_variables[0].value == "42";
}

bool evaluate_template(void *, fiber::http::HttpExchange &exchange, std::span<const PathVariable> path_variables,
                       std::string_view, const void *, std::string_view expression, std::string &output,
                       AccessError &error) noexcept {
    if (expression == "fail") {
        error = AccessError::template_script("fixture failure");
        return false;
    }
    if (expression == "$request.method") {
        output.assign(exchange.method_view());
        return true;
    }
    if (expression == "$path.id" && path_variables.size() == 1) {
        output.assign(path_variables[0].value);
        return true;
    }
    output.clear();
    return true;
}

AccessRequestScriptAdapter script_adapter() {
    return AccessRequestScriptAdapter{
            .evaluate_condition = evaluate_condition,
            .evaluate_template = evaluate_template,
    };
}

struct CapturedProxyRequest {
    std::string service;
    std::optional<std::string> cluster;
    std::string project;
    std::string initial_context_cluster;
    std::string origin_host;
    fiber::http::HttpMethod method = fiber::http::HttpMethod::Unknown;
    std::string received_body;
    std::size_t max_request_body_size = 0;
    std::int32_t timeout_millis = 0;
    std::optional<std::int64_t> max_response_body_size;
    std::optional<std::int32_t> websocket_timeout_millis;
    std::size_t proxy_header_count = 0;
    std::size_t context_count = 0;
    bool rewrite_configured = false;
    bool flush = false;
    bool template_evaluator_configured = false;
};

fiber::async::Task<fiber::common::IoResult<void>>
capture_proxy_request(void *context, fiber::http::HttpExchange &exchange,
                      const fiber::access_server::CompiledProxyRoute &proxy, ProxyExecutionInput input,
                      std::span<const fiber::access_server::EvaluatedHeader> base_headers,
                      fiber::access_server::AccessRequestTelemetry *) noexcept {
    auto &capture = *static_cast<CapturedProxyRequest *>(context);
    capture.service.assign(proxy.address_selector ? proxy.address_selector->service_name() : std::string_view{});
    const std::optional<std::string_view> configured_cluster =
            proxy.address_selector ? proxy.address_selector->configured_cluster() : std::nullopt;
    if (configured_cluster) {
        capture.cluster = std::string(*configured_cluster);
    } else {
        capture.cluster.reset();
    }
    capture.project.assign(input.project);
    capture.initial_context_cluster.assign(input.initial_context_cluster);
    capture.origin_host.assign(input.origin_host);
    capture.method = exchange.method();
    capture.max_request_body_size = input.max_request_body_size;
    capture.timeout_millis = proxy.timeout_millis;
    capture.max_response_body_size = proxy.max_response_body_size;
    capture.websocket_timeout_millis = proxy.websocket_timeout_millis;
    capture.proxy_header_count = proxy.proxy_headers.size();
    capture.context_count = proxy.context.size();
    capture.rewrite_configured = proxy.rewrite.has_value();
    capture.flush = proxy.flush.value_or(false);
    capture.template_evaluator_configured = input.template_evaluator.evaluate != nullptr;
    capture.received_body.clear();

    if (!exchange.request_body_spec().is_none()) {
        for (;;) {
            auto body = co_await exchange.read_body(64 * 1024);
            if (!body) {
                co_return std::unexpected(body.error());
            }
            const bool complete = body->complete();
            while (const fiber::mem::IoBuf *part = body->first_readable()) {
                capture.received_body.append(reinterpret_cast<const char *>(part->readable_data()), part->readable());
                body->consume_and_compact(part->readable());
            }
            if (complete) {
                break;
            }
        }
    }

    constexpr std::string_view kBody = "proxied";
    fiber::http::HttpHeaders response_headers(exchange.pool());
    for (const auto &header: base_headers) {
        if (!response_headers.set(header.name, header.value)) {
            co_return std::unexpected(fiber::common::IoErr::NoMem);
        }
    }
    if (!response_headers.set("X-Proxy-Fixture", "captured")) {
        co_return std::unexpected(fiber::common::IoErr::NoMem);
    }
    auto sent_header = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &response_headers,
            .body = fiber::http::HttpBodySpec::ContentLength(kBody.size()),
            .connection_mode = fiber::http::ResponseConnectionMode::Auto,
            .end_stream = false,
    });
    if (!sent_header) {
        co_return std::unexpected(sent_header.error());
    }
    auto sent_body =
            co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(kBody.data()), kBody.size(), true);
    if (!sent_body) {
        co_return std::unexpected(sent_body.error());
    }
    if (*sent_body != kBody.size()) {
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }
    co_return fiber::common::IoResult<void>{};
}

AccessProxyAdapter proxy_adapter(CapturedProxyRequest &capture) {
    return AccessProxyAdapter{
            .context = &capture,
            .execute = capture_proxy_request,
    };
}

TEST(AccessRequestHandlerTest, WritesLiveResponseAndExposesPathVariablesToScripts) {
    RouteConfig conditional = response_route("/items/:id", "item=${$path.id};method=${$request.method}", 201);
    conditional.condition = "id-is-42";
    conditional.response_headers = {
            StringConfigEntry{.name = "X-Item", .value = "${$path.id}"},
            StringConfigEntry{.name = "Content-Length", .value = "999"},
            StringConfigEntry{.name = "Strict-Transport-Security", .value = "override"},
    };
    RouteConfig fallback = response_route("/items/:id", "fallback");

    RouteConfigStore store;
    publish(store, project({}, {std::move(conditional), std::move(fallback)}));

    const std::string response = run_request(store,
                                             "POST /items/42 HTTP/1.1\r\n"
                                             "Host: api.example.com\r\n"
                                             "X-Forwarded-Proto: https\r\n"
                                             "Content-Length: 3\r\n"
                                             "Connection: close\r\n\r\n"
                                             "abc",
                                             script_adapter());

    EXPECT_TRUE(response.starts_with("HTTP/1.1 201 Created\r\n"));
    EXPECT_NE(response.find("X-Item: 42\r\n"), std::string::npos);
    EXPECT_NE(response.find("Strict-Transport-Security: override\r\n"), std::string::npos);
    EXPECT_NE(response.find("Content-Length: 19\r\n"), std::string::npos);
    EXPECT_EQ(response_body(response), "item=42;method=POST");
}

TEST(AccessRequestHandlerTest, SkipsConditionalRouteWithoutScriptAdapter) {
    RouteConfig conditional = response_route("/items/:id", "conditional");
    conditional.condition = "true";
    RouteConfig fallback = response_route("/items/:id", "fallback");

    RouteConfigStore store;
    publish(store, project({}, {std::move(conditional), std::move(fallback)}));

    const std::string response = run_request(store, "GET /items/42 HTTP/1.1\r\n"
                                                    "Host: api.example.com\r\n"
                                                    "Connection: close\r\n\r\n");

    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_EQ(response_body(response), "fallback");
}

TEST(AccessRequestHandlerTest, ExecutesPrecompiledLocalConditionAndTemplates) {
    AccessScriptRuntime scripts;
    RouteConfig conditional = response_route(
            "/local/:id", "id=${$path.id};method=${$req.method};query=${$query.q};header=${$header.x_test}", 202);
    conditional.condition = "$path.id === '42'";
    RouteConfig fallback = response_route("/local/:id", "fallback");

    RouteConfigStore store(scripts.compiler_adapter());
    publish(store, project({}, {std::move(conditional), std::move(fallback)}));

    const std::string matched = run_request(store,
                                            "GET /local/42?q=ok HTTP/1.1\r\n"
                                            "Host: api.example.com\r\n"
                                            "X-Test: yes\r\n"
                                            "Connection: close\r\n\r\n",
                                            scripts.request_adapter());
    EXPECT_TRUE(matched.starts_with("HTTP/1.1 202 Accepted\r\n"));
    EXPECT_EQ(response_body(matched), "id=42;method=GET;query=ok;header=yes");

    const std::string fallback_response = run_request(store,
                                                      "GET /local/41 HTTP/1.1\r\n"
                                                      "Host: api.example.com\r\n"
                                                      "Connection: close\r\n\r\n",
                                                      scripts.request_adapter());
    EXPECT_TRUE(fallback_response.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_EQ(response_body(fallback_response), "fallback");
}

TEST(AccessRequestHandlerTest, MatchesProductionConditionAndTemplateCorpus) {
    AccessScriptRuntime scripts;
    RouteConfig conditional =
            response_route("/corpus/*tail",
                           "tail=${$path.tail};host=${$header.host};host_case=${$header.Host};"
                           "header_default=${$header.hi_trace_cluster || 'stable'};"
                           "context=${$context.hi_trace_cluster || 'stable'};"
                           "cookie=${$cookie.cluster || 'stable'};query=${$query.redirect};"
                           "request_path=${$req.path};request_query=${$req.query};request_method=${$req.method};"
                           "origin=${$header.origin};forwarded_for=${$header.proxy_add_x_forwarded_for};"
                           "remote_addr=${$header.remote_addr}",
                           203);
    conditional.condition = "$header.hi_trace_cluster == 'header-blue' && $header.connection == 'close' && "
                            "$header.x_entry != 'internet' && $req.method == 'GET' && "
                            "strings.hasPrefix($path.tail, 'segment') && rand.random(1) <= 0";
    RouteConfig fallback = response_route("/corpus/*tail", "fallback");

    RouteConfigStore store(scripts.compiler_adapter());
    publish(store, project({}, {std::move(conditional), std::move(fallback)}));

    AccessRequestHandlerOptions options;
    options.test_mode = true;
    const std::string response = run_request(store,
                                             "GET /corpus/segment/rest?redirect=%2Fnext HTTP/1.1\r\n"
                                             "Host: api_context-blue.example.com\r\n"
                                             "HI-TRACE-CLUSTER: header-blue\r\n"
                                             "X-Entry: desktop\r\n"
                                             "Origin: https://origin.example.test\r\n"
                                             "Proxy-Add-X-Forwarded-For: 192.0.2.20\r\n"
                                             "Remote-Addr: 192.0.2.10\r\n"
                                             "Cookie: cluster=cookie-blue\r\n"
                                             "Connection: close\r\n\r\n",
                                             scripts.request_adapter(), options);

    EXPECT_TRUE(response.starts_with("HTTP/1.1 203 "));
    EXPECT_EQ(response_body(response),
              "tail=segment/rest;host=api_context-blue.example.com;host_case=api_context-blue.example.com;"
              "header_default=header-blue;context=context-blue;cookie=cookie-blue;query=/next;"
              "request_path=/corpus/segment/rest;request_query=redirect=%2Fnext;request_method=GET;"
              "origin=https://origin.example.test;forwarded_for=192.0.2.20;remote_addr=192.0.2.10");
}

TEST(AccessRequestHandlerTest, ReturnsJavaHostEntryPathAndCidrErrors) {
    HostStrategyConfig strategy;
    strategy.net_mask = fiber::access_server::kNetVdi;
    RouteConfig route = response_route("/allowed", "ok");
    route.allows = {
            std::optional<std::string>("10.0.0.0/8"),
    };
    RouteConfigStore store;
    publish(store, project(strategy, {std::move(route)}));

    const std::string host_error = run_request(store, "GET /allowed HTTP/1.1\r\n"
                                                      "Host: missing.example.com\r\n"
                                                      "Connection: close\r\n\r\n");
    EXPECT_TRUE(host_error.starts_with("HTTP/1.1 404 Not Found\r\n"));
    EXPECT_EQ(host_error.find("Strict-Transport-Security"), std::string::npos);
    EXPECT_EQ(response_body(host_error), R"({"name":"ROUTER_NOT_FOUND","message":"error find router","meta":null})");

    const std::string entry_error = run_request(store, "GET /allowed HTTP/1.1\r\n"
                                                       "Host: api.example.com\r\n"
                                                       "X-Entry: desktop\r\n"
                                                       "Connection: close\r\n\r\n");
    EXPECT_TRUE(entry_error.starts_with("HTTP/1.1 403 Forbidden\r\n"));
    EXPECT_NE(entry_error.find("Strict-Transport-Security: max-age=31536000\r\n"), std::string::npos);
    EXPECT_EQ(response_body(entry_error), R"({"name":"ENTRY_ERROR","message":"entry error","meta":null})");

    const std::string path_error = run_request(store, "GET /missing HTTP/1.1\r\n"
                                                      "Host: api.example.com\r\n"
                                                      "X-Entry: vdi\r\n"
                                                      "Connection: close\r\n\r\n");
    EXPECT_TRUE(path_error.starts_with("HTTP/1.1 404 Not Found\r\n"));
    EXPECT_EQ(response_body(path_error),
              R"({"name":"URL_NOT_MATCHED","message":"url not matched is project:orders","meta":null})");

    const std::string cidr_error = run_request(store, "GET /allowed HTTP/1.1\r\n"
                                                      "Host: api.example.com\r\n"
                                                      "X-Entry: vdi\r\n"
                                                      "X-Real-Ip: 192.168.1.1:8080\r\n"
                                                      "Connection: close\r\n\r\n");
    EXPECT_TRUE(cidr_error.starts_with("HTTP/1.1 403 Forbidden\r\n"));
    EXPECT_EQ(response_body(cidr_error), R"({"name":"NOT_ALLOW_IP","message":"source ip is not allowed","meta":null})");
}

TEST(AccessRequestHandlerTest, RedirectsBeforePathMatching) {
    HostStrategyConfig strategy;
    strategy.https = HttpsStrategy::Redirect307;
    RouteConfigStore store;
    publish(store, project(strategy, {response_route("/never", "never")}));

    const std::string response = run_request(store, "GET /missing?q=1 HTTP/1.1\r\n"
                                                    "Host: api.example.com:8080\r\n"
                                                    "Connection: close\r\n\r\n");

    EXPECT_TRUE(response.starts_with("HTTP/1.1 307 "));
    EXPECT_NE(response.find("Strict-Transport-Security: max-age=31536000\r\n"), std::string::npos);
    EXPECT_NE(response.find("Location: https://api.example.com:8080/missing?q=1\r\n"), std::string::npos);
    EXPECT_EQ(response_body(response), "");
}

TEST(AccessRequestHandlerTest, PreservesJavaHeaderCommitBoundaryOnLiveErrors) {
    RouteConfig header_failure = response_route("/header-failure", "unreached");
    header_failure.response_headers = {
            StringConfigEntry{.name = "X-First", .value = "one"},
            StringConfigEntry{.name = "X-Fail", .value = "${fail}"},
    };
    RouteConfig body_failure = response_route("/body-failure", "${fail}");
    body_failure.response_headers = {
            StringConfigEntry{.name = "X-First", .value = "one"},
    };
    RouteConfigStore store;
    publish(store, project({}, {std::move(header_failure), std::move(body_failure)}));

    const std::string header_response = run_request(store,
                                                    "GET /header-failure HTTP/1.1\r\n"
                                                    "Host: api.example.com\r\n"
                                                    "Connection: close\r\n\r\n",
                                                    script_adapter());
    EXPECT_TRUE(header_response.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
    EXPECT_EQ(header_response.find("X-First: one\r\n"), std::string::npos);
    EXPECT_NE(header_response.find("Strict-Transport-Security: max-age=31536000\r\n"), std::string::npos);

    const std::string body_response = run_request(store,
                                                  "GET /body-failure HTTP/1.1\r\n"
                                                  "Host: api.example.com\r\n"
                                                  "Connection: close\r\n\r\n",
                                                  script_adapter());
    EXPECT_TRUE(body_response.starts_with("HTTP/1.1 500 Internal Server Error\r\n"));
    EXPECT_NE(body_response.find("X-First: one\r\n"), std::string::npos);
    EXPECT_NE(body_response.find("Strict-Transport-Security: max-age=31536000\r\n"), std::string::npos);
}

TEST(AccessRequestHandlerTest, ChecksKnownBodyLengthBeforeCidr) {
    RouteConfig route = response_route("/limited", "unreached");
    route.allows = {
            std::optional<std::string>("10.0.0.0/8"),
    };
    RouteConfigStore store;
    publish(store, project({}, {std::move(route)}));

    AccessRequestHandlerOptions options;
    options.default_max_request_body_size = 4;
    const std::string response = run_request(store,
                                             "POST /limited HTTP/1.1\r\n"
                                             "Host: api.example.com\r\n"
                                             "X-Real-Ip: 192.168.1.1\r\n"
                                             "Content-Length: 5\r\n"
                                             "Connection: close\r\n\r\n"
                                             "12345",
                                             {}, options);

    EXPECT_TRUE(response.starts_with("HTTP/1.1 413 Payload Too Large\r\n"));
    EXPECT_EQ(response_body(response),
              R"({"name":"REQ_BODY_TOO_LARGE","message":"request body is too large","meta":null})");
}

TEST(AccessRequestHandlerTest, AppliesRouteBodyLimitAndJavaNegativeUnlimitedRule) {
    RouteConfig positive = response_route("/positive", "positive");
    positive.max_client_body_size = 6;
    RouteConfig unlimited = response_route("/unlimited", "unlimited");
    unlimited.max_client_body_size = -1;
    RouteConfigStore store;
    publish(store, project({}, {std::move(positive), std::move(unlimited)}));

    AccessRequestHandlerOptions options;
    options.default_max_request_body_size = 4;
    const std::string positive_response = run_request(store,
                                                      "POST /positive HTTP/1.1\r\n"
                                                      "Host: api.example.com\r\n"
                                                      "Content-Length: 5\r\n"
                                                      "Connection: close\r\n\r\n"
                                                      "12345",
                                                      {}, options);
    EXPECT_TRUE(positive_response.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_EQ(response_body(positive_response), "positive");

    const std::string unlimited_response = run_request(store,
                                                       "POST /unlimited HTTP/1.1\r\n"
                                                       "Host: api.example.com\r\n"
                                                       "Content-Length: 5\r\n"
                                                       "Connection: close\r\n\r\n"
                                                       "12345",
                                                       {}, options);
    EXPECT_TRUE(unlimited_response.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_EQ(response_body(unlimited_response), "unlimited");
}

TEST(AccessRequestHandlerTest, DoesNotWriteAnErrorAfterResponseCommit) {
    const std::string response = run_committed_response();

    EXPECT_TRUE(response.starts_with("HTTP/1.1 204 No Content\r\n"));
    EXPECT_EQ(std::count(response.begin(), response.end(), '{'), 0);
    EXPECT_EQ(response.find("ENTRY_ERROR"), std::string::npos);
    EXPECT_EQ(response.find("HTTP/1.1", std::string_view("HTTP/1.1").size()), std::string::npos);
}

TEST(AccessRequestHandlerTest, PassesPinnedProxyRouteAndExecutionInputToAdapter) {
    RouteConfig route = proxy_route("/v1/:id");
    route.cluster = "stable";
    route.timeout_millis = 123;
    route.max_client_body_size = 10;
    route.max_proxy_body_size = -1;
    route.websocket_timeout_millis = 500;
    route.flush = true;
    route.rewrite = "/items/${$path.id} /?#";
    route.proxy_headers = {
            StringConfigEntry{.name = "X-Item", .value = "${$path.id}"},
            StringConfigEntry{.name = "X-Empty", .value = "${empty}"},
            StringConfigEntry{.name = "Host", .value = "internal.example:8080"},
            StringConfigEntry{.name = "Connection", .value = "keep-alive"},
            StringConfigEntry{.name = "Content-Length", .value = "999"},
            StringConfigEntry{.name = "X-Override", .value = "configured"},
            StringConfigEntry{.name = "X-Ploto-Source-App", .value = "spoofed"},
    };
    route.context = {
            StringConfigEntry{.name = "cluster", .value = "${$path.id}"},
            StringConfigEntry{.name = "remove-me", .value = "${empty}"},
    };

    RouteConfigStore store;
    publish(store, project({}, {std::move(route)}));

    CapturedProxyRequest capture;
    const std::string response = run_request(store,
                                             "POST /v1/42?raw=%2F HTTP/1.1\r\n"
                                             "Host: api.example.com\r\n"
                                             "Content-Length: 4\r\n"
                                             "Connection: close\r\n"
                                             "X-Override: incoming\r\n"
                                             "x-empty: incoming\r\n"
                                             "X-Incoming: one\r\n"
                                             "X-Incoming: two\r\n"
                                             "X-Ploto-Source-App: incoming\r\n\r\n"
                                             "body",
                                             script_adapter(), {}, proxy_adapter(capture));

    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(response.find("Strict-Transport-Security: max-age=31536000\r\n"), std::string::npos);
    EXPECT_EQ(response_body(response), "proxied");
    EXPECT_EQ(capture.service, "orders");
    ASSERT_TRUE(capture.cluster);
    EXPECT_EQ(*capture.cluster, "stable");
    EXPECT_EQ(capture.project, "orders");
    EXPECT_TRUE(capture.initial_context_cluster.empty());
    EXPECT_TRUE(capture.origin_host.empty());
    EXPECT_EQ(capture.method, fiber::http::HttpMethod::Post);
    EXPECT_EQ(capture.received_body, "body");
    EXPECT_EQ(capture.max_request_body_size, 10U);
    EXPECT_EQ(capture.timeout_millis, 123);
    ASSERT_TRUE(capture.max_response_body_size);
    EXPECT_EQ(*capture.max_response_body_size, -1);
    ASSERT_TRUE(capture.websocket_timeout_millis);
    EXPECT_EQ(*capture.websocket_timeout_millis, 500);
    EXPECT_EQ(capture.proxy_header_count, 7U);
    EXPECT_EQ(capture.context_count, 2U);
    EXPECT_TRUE(capture.rewrite_configured);
    EXPECT_TRUE(capture.flush);
    EXPECT_TRUE(capture.template_evaluator_configured);
}

TEST(AccessRequestHandlerTest, PassesJavaTestHostClusterAndOriginHostToProxyExecutor) {
    AccessScriptRuntime scripts;
    RouteConfig route = proxy_route("/cluster");
    route.proxy_headers = {
            StringConfigEntry{.name = "X-Request-Cluster", .value = "${$context.cluster}"},
    };
    RouteConfigStore store(scripts.compiler_adapter());
    publish(store, project({}, {std::move(route)}));

    CapturedProxyRequest capture;
    AccessRequestHandlerOptions options;
    options.test_mode = true;
    const std::string response = run_request(store,
                                             "GET /cluster HTTP/1.1\r\n"
                                             "Host: api_gray.example.com\r\n"
                                             "Connection: close\r\n\r\n",
                                             scripts.request_adapter(), options, proxy_adapter(capture));

    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_EQ(capture.initial_context_cluster, "gray");
    EXPECT_EQ(capture.origin_host, "api_gray.example.com");
}

TEST(AccessRequestHandlerTest, UsesTestTraceHeaderWhenHostHasNoCluster) {
    RouteConfig route = proxy_route("/cluster");
    RouteConfigStore store;
    publish(store, project({}, {std::move(route)}));

    CapturedProxyRequest capture;
    AccessRequestHandlerOptions options;
    options.test_mode = true;
    const std::string response = run_request(store,
                                             "GET /cluster HTTP/1.1\r\n"
                                             "Host: api.example.com\r\n"
                                             "HI-TRACE-CLUSTER: canary\r\n"
                                             "Connection: close\r\n\r\n",
                                             {}, options, proxy_adapter(capture));

    EXPECT_TRUE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    EXPECT_EQ(capture.initial_context_cluster, "canary");
    EXPECT_TRUE(capture.origin_host.empty());
}

} // namespace
