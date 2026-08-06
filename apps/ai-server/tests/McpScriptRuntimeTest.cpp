#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>

#include <cerrno>
#include <sys/socket.h>

#include <async/Spawn.h>
#include <common/IoError.h>
#include <common/mem/BufPool.h>
#include <common/mem/IoBufChain.h>
#include <event/EventLoop.h>
#include <event/EventLoopGroup.h>
#include <http/ClientHttp1Exchange.h>
#include <http/Http1ClientConnection.h>
#include <http/Http1Server.h>
#include <http/HttpBodySpec.h>
#include <http/HttpExchange.h>
#include <http/HttpHeaders.h>
#include <http_script/HttpScriptServices.h>
#include <net/SocketAddress.h>

#include "mcp/McpScriptRuntime.h"

namespace {

using namespace std::chrono_literals;

fiber::common::IoResult<std::uint16_t> bound_port(int fd) noexcept {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&storage), &length) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&storage), length, address)) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    return address.port();
}

fiber::async::Task<fiber::common::IoResult<std::string>> read_body(fiber::http::HttpExchange &exchange) noexcept {
    std::string body;
    for (;;) {
        auto chunk = co_await exchange.read_body(64 * 1024);
        if (!chunk) {
            co_return std::unexpected(chunk.error());
        }
        const bool complete = chunk->complete();
        while (const fiber::mem::IoBuf *part = chunk->first_readable()) {
            body.append(reinterpret_cast<const char *>(part->readable_data()), part->readable());
            chunk->consume_and_compact(part->readable());
        }
        if (complete) {
            break;
        }
    }
    co_return body;
}

class TestUpstreamConnection final : public fiber::http_script::HttpUpstreamConnection {
public:
    TestUpstreamConnection(std::unique_ptr<fiber::http::Http1ClientConnection> connection,
                           std::string authority) noexcept :
        connection_(std::move(connection)), authority_(std::move(authority)) {}

    fiber::http::Http1ClientConnection &connection() noexcept override { return *connection_; }
    std::string_view authority() const noexcept override { return authority_; }

private:
    std::unique_ptr<fiber::http::Http1ClientConnection> connection_;
    std::string authority_;
};

class TestScriptServices final : public fiber::http_script::HttpScriptServices {
public:
    explicit TestScriptServices(std::uint16_t port) noexcept : port_(port) {}

    fiber::async::Task<fiber::common::IoResult<std::unique_ptr<fiber::http_script::HttpUpstreamConnection>>>
    acquire(const fiber::http_script::HttpTargetSpec &, std::chrono::milliseconds timeout) noexcept override {
        auto connection = std::make_unique<fiber::http::Http1ClientConnection>(
                fiber::event::EventLoop::current(),
                fiber::http::Http1ClientConnectionOptions{
                        .peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port_),
                });
        auto connected = co_await connection->connect(timeout);
        if (!connected) {
            co_return std::unexpected(connected.error());
        }
        std::string authority = "127.0.0.1:";
        authority.append(std::to_string(port_));
        co_return std::unique_ptr<fiber::http_script::HttpUpstreamConnection>(
                new TestUpstreamConnection(std::move(connection), std::move(authority)));
    }

private:
    std::uint16_t port_ = 0;
};

class JsonServiceFixture final {
public:
    JsonServiceFixture() :
        server_(group_.at(0), [this](fiber::http::HttpExchange &exchange) { return handle(exchange); }) {
        group_.start();
        std::promise<std::uint16_t> promise;
        auto future = promise.get_future();
        fiber::async::spawn(group_.at(0), [this, &promise]() -> fiber::async::DetachedTask {
            if (!server_.bind({fiber::net::IpAddress::loopback_v4(), 0}, {})) {
                promise.set_value(0);
                co_return;
            }
            auto port = bound_port(server_.fd());
            promise.set_value(port ? *port : 0);
            fiber::async::spawn([this]() { return server_.serve(); });
        });
        if (future.wait_for(5s) == std::future_status::ready) {
            port_ = future.get();
        }
    }

    ~JsonServiceFixture() {
        std::promise<void> promise;
        auto future = promise.get_future();
        fiber::async::spawn(group_.at(0), [this, &promise]() -> fiber::async::DetachedTask {
            server_.close();
            co_await server_.shutdown_and_wait();
            promise.set_value();
        });
        (void) future.wait_for(5s);
        group_.stop();
        group_.join();
    }

    std::uint16_t port() const noexcept { return port_; }

private:
    fiber::async::Task<void> handle(fiber::http::HttpExchange &exchange) noexcept {
        auto body = co_await read_body(exchange);
        const std::string_view path = exchange.uri().path;
        bool valid = body.has_value() && !exchange.header("host").empty();
        if (path == "/post-json") {
            valid = valid && exchange.method() == fiber::http::HttpMethod::Post && *body == R"({"x":7})" &&
                    exchange.header("content-type").find("application/json") != std::string_view::npos;
        } else if (path == "/post-form") {
            valid = valid && exchange.method() == fiber::http::HttpMethod::Post && *body == "a=1" &&
                    exchange.header("content-type") == "application/x-www-form-urlencoded";
        } else if (path == "/get") {
            valid = valid && exchange.method() == fiber::http::HttpMethod::Get && body->empty();
        } else {
            valid = false;
        }
        const std::string_view response = valid ? R"({"ok":true})" : R"({"ok":false})";
        fiber::http::HttpHeaders headers(exchange.pool());
        headers.set_view("Content-Type", "application/json;charset=utf-8");
        auto sent = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = valid ? 200 : 400,
                .headers = &headers,
                .body = fiber::http::HttpBodySpec::ContentLength(response.size()),
                .connection_mode = fiber::http::ResponseConnectionMode::Auto,
                .end_stream = false,
        });
        if (sent) {
            (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(response.data()), response.size(),
                                               true);
        }
    }

    fiber::event::EventLoopGroup group_{1};
    fiber::http::Http1Server server_;
    std::uint16_t port_ = 0;
};

} // namespace

TEST(McpScriptRuntimeTest, CompilesAndExecutesJsonAndVoidResults) {
    auto compiled = fiber::ai_server::compile_mcp_tool_script("return {echo: $.message, ok: true};");
    ASSERT_TRUE(compiled);
    auto void_script = fiber::ai_server::compile_mcp_tool_script("return;");
    ASSERT_TRUE(void_script);

    fiber::event::EventLoop loop;
    fiber::mem::IoBufNodePool nodes;
    fiber::http::HttpExchange exchange(nodes, {}, {});
    bool completed = false;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto result = co_await (*compiled)->invoke(exchange, R"({"message":"hello"})");
        EXPECT_TRUE(result.has_content);
        EXPECT_FALSE(result.is_error);
        EXPECT_EQ(result.text, R"({"echo":"hello","ok":true})");

        auto empty = co_await (*void_script)->invoke(exchange, "{}");
        EXPECT_FALSE(empty.has_content);
        EXPECT_FALSE(empty.is_error);
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(McpScriptRuntimeTest, ReportsCompileAndScriptErrors) {
    auto invalid = fiber::ai_server::compile_mcp_tool_script("return {");
    ASSERT_FALSE(invalid);
    EXPECT_FALSE(invalid.error().message.empty());

    auto compiled = fiber::ai_server::compile_mcp_tool_script("throw 'tool failed';");
    ASSERT_TRUE(compiled);
    fiber::event::EventLoop loop;
    fiber::mem::IoBufNodePool nodes;
    fiber::http::HttpExchange exchange(nodes, {}, {});
    bool completed = false;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto result = co_await (*compiled)->invoke(exchange, "{}");
        EXPECT_TRUE(result.has_content);
        EXPECT_TRUE(result.is_error);
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(McpScriptRuntimeTest, AcceptsSourceServiceAndAddressDirectives) {
    auto service = fiber::ai_server::compile_mcp_tool_script(
            R"(directive users = service "user/default"; return users.postJson("/users", $);)");
    EXPECT_TRUE(service) << (service ? "" : service.error().message);

    auto address = fiber::ai_server::compile_mcp_tool_script(
            R"(directive users = address "http://127.0.0.1:8080"; return users.getJson("/users");)");
    EXPECT_TRUE(address) << (address ? "" : address.error().message);

    auto form = fiber::ai_server::compile_mcp_tool_script(
            R"(directive users = address "http://127.0.0.1:8080"; return users.postForm("/users", $);)");
    EXPECT_TRUE(form) << (form ? "" : form.error().message);
}

TEST(McpScriptRuntimeTest, ExecutesSourceJsonAndFormHttpHelpers) {
    JsonServiceFixture fixture;
    ASSERT_NE(fixture.port(), 0);
    TestScriptServices services(fixture.port());
    auto post_json = fiber::ai_server::compile_mcp_tool_script(
            R"(directive api = address "http://127.0.0.1:1"; return api.postJson("/post-json", $);)", &services);
    auto post_form = fiber::ai_server::compile_mcp_tool_script(
            R"(directive api = address "http://127.0.0.1:1"; return api.postForm("/post-form", {a: 1});)", &services);
    auto get_json = fiber::ai_server::compile_mcp_tool_script(
            R"(directive api = address "http://127.0.0.1:1"; return api.getJson("/get");)", &services);
    ASSERT_TRUE(post_json);
    ASSERT_TRUE(post_form);
    ASSERT_TRUE(get_json);

    fiber::event::EventLoop loop;
    fiber::mem::IoBufNodePool nodes;
    fiber::http::HttpExchange exchange(nodes, {}, {});
    bool completed = false;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        const auto json_result = co_await (*post_json)->invoke(exchange, R"({"x":7})");
        EXPECT_FALSE(json_result.is_error);
        EXPECT_EQ(json_result.text, R"({"ok":true})");
        const auto form_result = co_await (*post_form)->invoke(exchange, "{}");
        EXPECT_FALSE(form_result.is_error);
        EXPECT_EQ(form_result.text, R"({"ok":true})");
        const auto get_result = co_await (*get_json)->invoke(exchange, "{}");
        EXPECT_FALSE(get_result.is_error);
        EXPECT_EQ(get_result.text, R"({"ok":true})");
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}
