#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <string_view>

#include <cerrno>
#include <sys/socket.h>

#include <async/Spawn.h>
#include <common/IoError.h>
#include <common/mem/BufPool.h>
#include <event/EventLoop.h>
#include <event/EventLoopGroup.h>
#include <http/ClientHttp1Exchange.h>
#include <http/ClientHttp1Types.h>
#include <http/Http1ClientConnection.h>
#include <http/Http1Server.h>
#include <http/HttpBodySpec.h>
#include <http/HttpExchange.h>
#include <http/HttpHeaders.h>
#include <net/SocketAddress.h>

#include "mcp/McpConfigSnapshot.h"
#include "mcp/McpSessionForwarder.h"
#include "mcp/McpSessionManager.h"
#include "server/McpHttpHandler.h"

namespace {

using namespace std::chrono_literals;

struct McpResponse {
    int status = 0;
    std::string session_id;
    std::string content_type;
    std::string body;
    fiber::common::IoErr error = fiber::common::IoErr::None;
};

struct LegacyMcpResponse {
    int stream_status = 0;
    int message_status = 0;
    std::string endpoint_event;
    std::string message_event;
    fiber::common::IoErr error = fiber::common::IoErr::None;
};

class IntegrationTool final : public fiber::ai_server::McpToolHandler {
public:
    fiber::async::Task<fiber::ai_server::McpToolCallResult>
    invoke(fiber::http::HttpExchange &, std::string_view arguments_json) const noexcept override {
        co_return fiber::ai_server::McpToolCallResult{
                .text = std::string(arguments_json),
                .has_content = true,
        };
    }
};

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

std::string consume(fiber::mem::IoBufChain chain) {
    std::string output;
    while (const fiber::mem::IoBuf *part = chain.first_readable()) {
        output.append(reinterpret_cast<const char *>(part->readable_data()), part->readable());
        chain.consume_and_compact(part->readable());
    }
    return output;
}

McpResponse request(std::uint16_t port, fiber::http::HttpMethod method, std::string_view target,
                    std::string_view body = {}, std::string_view session_id = {},
                    std::string_view protocol_version = {}) {
    fiber::event::EventLoop loop;
    std::promise<McpResponse> promise;
    auto future = promise.get_future();
    fiber::async::spawn(
            loop,
            [&loop, port, method, target = std::string(target), body = std::string(body),
             session_id = std::string(session_id), protocol_version = std::string(protocol_version),
             &promise]() mutable -> fiber::async::DetachedTask {
                McpResponse response;
                fiber::http::Http1ClientConnection connection(
                        loop,
                        fiber::http::Http1ClientConnectionOptions{
                                .peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port),
                        });
                auto connected = co_await connection.connect(5s);
                if (!connected) {
                    response.error = connected.error();
                    promise.set_value(std::move(response));
                    loop.stop();
                    co_return;
                }
                fiber::mem::BufPool pool;
                fiber::http::HttpHeaders headers(pool);
                headers.set_view("Host", "127.0.0.1");
                headers.set_view("Accept", "application/json, text/event-stream");
                if (!body.empty()) {
                    headers.set_view("Content-Type", "application/json");
                }
                if (!session_id.empty()) {
                    headers.set("Mcp-Session-Id", session_id);
                    headers.set("Mcp-Protocol-Version", protocol_version.empty() ? std::string_view("2025-03-26")
                                                                                 : std::string_view(protocol_version));
                } else if (!protocol_version.empty()) {
                    headers.set("Mcp-Protocol-Version", protocol_version);
                }
                fiber::http::ClientHttp1Exchange exchange(connection, pool);
                auto sent = co_await exchange.send_header(
                        {
                                .method = method,
                                .target = target,
                                .headers = &headers,
                                .body = body.empty() ? fiber::http::HttpBodySpec::None()
                                                     : fiber::http::HttpBodySpec::ContentLength(body.size()),
                        },
                        body.empty(), 5s);
                if (!sent) {
                    response.error = sent.error();
                    promise.set_value(std::move(response));
                    loop.stop();
                    co_return;
                }
                if (!body.empty()) {
                    auto written = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(body.data()),
                                                               body.size(), true, 5s);
                    if (!written) {
                        response.error = written.error();
                        promise.set_value(std::move(response));
                        loop.stop();
                        co_return;
                    }
                }
                const fiber::http::Http1ResponseHead *head = nullptr;
                for (;;) {
                    auto read = co_await exchange.read_header(5s);
                    if (!read) {
                        response.error = read.error();
                        promise.set_value(std::move(response));
                        loop.stop();
                        co_return;
                    }
                    if (!(*read)->is_informational()) {
                        head = *read;
                        break;
                    }
                }
                response.status = head->status_code;
                response.session_id = std::string(head->headers.get("mcp-session-id"));
                response.content_type = std::string(head->headers.get("content-type"));
                for (;;) {
                    auto chunk = co_await exchange.read_body(64 * 1024, 5s);
                    if (!chunk) {
                        response.error = chunk.error();
                        break;
                    }
                    const bool complete = chunk->complete();
                    response.body.append(consume(std::move(*chunk)));
                    if (complete) {
                        break;
                    }
                }
                promise.set_value(std::move(response));
                loop.stop();
            });
    loop.run();
    return future.get();
}

LegacyMcpResponse legacy_roundtrip(std::uint16_t port) {
    fiber::event::EventLoop loop;
    std::promise<LegacyMcpResponse> promise;
    auto future = promise.get_future();
    fiber::async::spawn(loop, [&loop, port, &promise]() -> fiber::async::DetachedTask {
        LegacyMcpResponse response;
        auto fail = [&](fiber::common::IoErr error) {
            response.error = error;
            promise.set_value(std::move(response));
            loop.stop();
        };

        fiber::http::Http1ClientConnection stream_connection(
                loop, fiber::http::Http1ClientConnectionOptions{
                              .peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port),
                      });
        auto connected = co_await stream_connection.connect(5s);
        if (!connected) {
            fail(connected.error());
            co_return;
        }
        fiber::mem::BufPool stream_pool;
        fiber::http::HttpHeaders stream_headers(stream_pool);
        stream_headers.set_view("Host", "127.0.0.1");
        stream_headers.set_view("Accept", "text/event-stream");
        fiber::http::ClientHttp1Exchange stream(stream_connection, stream_pool);
        auto sent = co_await stream.send_header(
                {
                        .method = fiber::http::HttpMethod::Get,
                        .target = "/demo/sse",
                        .headers = &stream_headers,
                        .body = fiber::http::HttpBodySpec::None(),
                },
                true, 5s);
        if (!sent) {
            fail(sent.error());
            co_return;
        }
        auto stream_head = co_await stream.read_header(5s);
        if (!stream_head) {
            fail(stream_head.error());
            co_return;
        }
        response.stream_status = (*stream_head)->status_code;
        const std::string session_id((*stream_head)->headers.get("mcp-session-id"));
        auto endpoint = co_await stream.read_body(64 * 1024, 5s);
        if (!endpoint) {
            fail(endpoint.error());
            co_return;
        }
        response.endpoint_event = consume(std::move(*endpoint));

        fiber::http::Http1ClientConnection message_connection(
                loop, fiber::http::Http1ClientConnectionOptions{
                              .peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port),
                      });
        connected = co_await message_connection.connect(5s);
        if (!connected) {
            fail(connected.error());
            co_return;
        }
        constexpr std::string_view kInitialize =
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})";
        fiber::mem::BufPool message_pool;
        fiber::http::HttpHeaders message_headers(message_pool);
        message_headers.set_view("Host", "127.0.0.1");
        message_headers.set_view("Content-Type", "application/json");
        std::string target = "/demo/message?sessionId=";
        target.append(session_id);
        fiber::http::ClientHttp1Exchange message(message_connection, message_pool);
        sent = co_await message.send_header(
                {
                        .method = fiber::http::HttpMethod::Post,
                        .target = target,
                        .headers = &message_headers,
                        .body = fiber::http::HttpBodySpec::ContentLength(kInitialize.size()),
                },
                false, 5s);
        if (!sent) {
            fail(sent.error());
            co_return;
        }
        auto written = co_await message.write_all(reinterpret_cast<const std::uint8_t *>(kInitialize.data()),
                                                  kInitialize.size(), true, 5s);
        if (!written) {
            fail(written.error());
            co_return;
        }
        auto message_head = co_await message.read_header(5s);
        if (!message_head) {
            fail(message_head.error());
            co_return;
        }
        response.message_status = (*message_head)->status_code;
        auto discarded = co_await message.discard_response_body(5s);
        if (!discarded) {
            fail(discarded.error());
            co_return;
        }

        for (std::size_t i = 0; i < 4 && response.message_event.find("event: message") == std::string::npos; ++i) {
            auto event = co_await stream.read_body(64 * 1024, 5s);
            if (!event) {
                fail(event.error());
                co_return;
            }
            response.message_event.append(consume(std::move(*event)));
        }
        (void) stream.abort();
        stream_connection.close();
        promise.set_value(std::move(response));
        loop.stop();
    });
    loop.run();
    return future.get();
}

class McpServerFixture final {
public:
    McpServerFixture() :
        sessions_("0100007f901f"), forwarder_(group_, ring_), proxy_sessions_("0200007f901f"),
        handler_(config_, sessions_), proxy_handler_(config_, proxy_sessions_, &forwarder_),
        server_(group_.at(0), [this](fiber::http::HttpExchange &exchange) { return handler_.handle(exchange); }),
        proxy_server_(group_.at(0),
                      [this](fiber::http::HttpExchange &exchange) { return proxy_handler_.handle(exchange); }) {
        auto project = std::make_shared<fiber::ai_server::McpProjectRuntime>();
        project->name = "demo";
        auto tool = std::make_shared<fiber::ai_server::McpTool>();
        tool->descriptor.script_id = "weather-tool";
        tool->descriptor.name = "weather";
        tool->descriptor.description = "read weather";
        tool->descriptor.input_schema_json = R"({"type":"object"})";
        tool->descriptor.tool_json =
                R"({"name":"weather","description":"read weather","inputSchema":{"type":"object"}})";
        tool->handler = std::make_shared<IntegrationTool>();
        project->tools.push_back(std::move(tool));
        auto snapshot = std::make_shared<fiber::ai_server::McpConfigSnapshot>();
        snapshot->generation = 1;
        snapshot->projects.push_back(std::move(project));
        config_.update(std::move(snapshot));
        group_.start();
        std::promise<std::pair<std::uint16_t, std::uint16_t>> promise;
        auto future = promise.get_future();
        fiber::async::spawn(group_.at(0), [this, &promise]() -> fiber::async::DetachedTask {
            if (!server_.bind({fiber::net::IpAddress::loopback_v4(), 0}, {})) {
                promise.set_value({0, 0});
                co_return;
            }
            auto port = bound_port(server_.fd());
            if (!port ||
                !ring_.update(1,
                              {
                                      fiber::ai_server::RateLimitNode{
                                              .node_id = "0100007f901f",
                                              .host = "127.0.0.1",
                                              .port = *port,
                                      },
                              }) ||
                !forwarder_.init() || !proxy_server_.bind({fiber::net::IpAddress::loopback_v4(), 0}, {})) {
                promise.set_value({0, 0});
                co_return;
            }
            auto proxy_port = bound_port(proxy_server_.fd());
            promise.set_value({*port, proxy_port ? *proxy_port : 0});
            fiber::async::spawn([this]() { return server_.serve(); });
            fiber::async::spawn([this]() { return proxy_server_.serve(); });
        });
        if (future.wait_for(5s) == std::future_status::ready) {
            const auto ports = future.get();
            port_ = ports.first;
            proxy_port_ = ports.second;
        }
    }

    ~McpServerFixture() {
        std::promise<void> promise;
        auto future = promise.get_future();
        fiber::async::spawn(group_.at(0), [this, &promise]() -> fiber::async::DetachedTask {
            sessions_.close_all();
            proxy_sessions_.close_all();
            server_.close();
            proxy_server_.close();
            co_await forwarder_.shutdown();
            co_await server_.shutdown_and_wait();
            co_await proxy_server_.shutdown_and_wait();
            promise.set_value();
        });
        (void) future.wait_for(5s);
        group_.stop();
        group_.join();
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::uint16_t proxy_port() const noexcept { return proxy_port_; }

private:
    fiber::event::EventLoopGroup group_{1};
    fiber::ai_server::McpConfigStore config_;
    fiber::ai_server::McpSessionManager sessions_;
    fiber::ai_server::RateLimitShardRing ring_;
    fiber::ai_server::McpSessionForwarder forwarder_;
    fiber::ai_server::McpSessionManager proxy_sessions_;
    fiber::ai_server::McpHttpHandler handler_;
    fiber::ai_server::McpHttpHandler proxy_handler_;
    fiber::http::Http1Server server_;
    fiber::http::Http1Server proxy_server_;
    std::uint16_t port_ = 0;
    std::uint16_t proxy_port_ = 0;
};

TEST(McpHttpIntegrationTest, InitializesSessionAndListsToolsOverStreamableHttp) {
    McpServerFixture fixture;
    ASSERT_NE(fixture.port(), 0);
    const auto initialized =
            request(fixture.port(), fiber::http::HttpMethod::Post, "/demo/mcp",
                    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})");
    ASSERT_EQ(initialized.error, fiber::common::IoErr::None);
    EXPECT_EQ(initialized.status, 200);
    EXPECT_EQ(initialized.content_type, "text/event-stream");
    ASSERT_TRUE(initialized.session_id.starts_with("0100007f901f"));
    EXPECT_NE(initialized.body.find("event: message"), std::string::npos);
    EXPECT_NE(initialized.body.find("\"protocolVersion\":\"2025-03-26\""), std::string::npos);

    const auto notification =
            request(fixture.port(), fiber::http::HttpMethod::Post, "/demo/mcp",
                    R"({"jsonrpc":"2.0","method":"notifications/initialized"})", initialized.session_id);
    EXPECT_EQ(notification.error, fiber::common::IoErr::None);
    EXPECT_EQ(notification.status, 202);

    const auto listed =
            request(fixture.port(), fiber::http::HttpMethod::Post, "/demo/mcp",
                    R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})", initialized.session_id);
    EXPECT_EQ(listed.error, fiber::common::IoErr::None);
    EXPECT_EQ(listed.status, 200);
    EXPECT_NE(listed.body.find("\"name\":\"weather\""), std::string::npos);

    const auto called = request(
            fixture.port(), fiber::http::HttpMethod::Post, "/demo/mcp",
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"weather","arguments":{"city":"Qingdao"}}})",
            initialized.session_id);
    EXPECT_EQ(called.error, fiber::common::IoErr::None);
    EXPECT_EQ(called.status, 200);
    EXPECT_NE(called.body.find("{\\\"city\\\":\\\"Qingdao\\\"}"), std::string::npos);
    EXPECT_NE(called.body.find("\"isError\":false"), std::string::npos);

    const auto deleted =
            request(fixture.port(), fiber::http::HttpMethod::Delete, "/demo/mcp", {}, initialized.session_id);
    EXPECT_EQ(deleted.error, fiber::common::IoErr::None);
    EXPECT_EQ(deleted.status, 200);
}

TEST(McpHttpIntegrationTest, RejectsUnknownProjectsInsideMcpRouter) {
    McpServerFixture fixture;
    ASSERT_NE(fixture.port(), 0);
    const auto response = request(fixture.port(), fiber::http::HttpMethod::Post, "/missing/mcp", "{}");
    EXPECT_EQ(response.error, fiber::common::IoErr::None);
    EXPECT_EQ(response.status, 404);
    EXPECT_NE(response.body.find("MCP project not found"), std::string::npos);
}

TEST(McpHttpIntegrationTest, InitializeNegotiatesVersionWithoutValidatingProtocolHeader) {
    McpServerFixture fixture;
    ASSERT_NE(fixture.port(), 0);
    const auto initialized =
            request(fixture.port(), fiber::http::HttpMethod::Post, "/demo/mcp",
                    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"unknown"}})", {},
                    "unknown-header");

    EXPECT_EQ(initialized.error, fiber::common::IoErr::None);
    EXPECT_EQ(initialized.status, 200);
    EXPECT_NE(initialized.body.find("\"protocolVersion\":\"2025-03-26\""), std::string::npos);
}

TEST(McpHttpIntegrationTest, LegacySsePublishesEndpointAndJsonRpcMessages) {
    McpServerFixture fixture;
    ASSERT_NE(fixture.port(), 0);

    const auto response = legacy_roundtrip(fixture.port());

    EXPECT_EQ(response.error, fiber::common::IoErr::None);
    EXPECT_EQ(response.stream_status, 200);
    EXPECT_EQ(response.message_status, 200);
    EXPECT_NE(response.endpoint_event.find("event: endpoint"), std::string::npos);
    EXPECT_NE(response.endpoint_event.find("/demo/message?sessionId="), std::string::npos);
    EXPECT_NE(response.message_event.find("event: message"), std::string::npos);
    EXPECT_NE(response.message_event.find("\"protocolVersion\":\"2025-03-26\""), std::string::npos);
}

TEST(McpHttpIntegrationTest, ForwardsRemoteSessionToOwningAiServer) {
    McpServerFixture fixture;
    ASSERT_NE(fixture.port(), 0);
    ASSERT_NE(fixture.proxy_port(), 0);
    const auto initialized =
            request(fixture.port(), fiber::http::HttpMethod::Post, "/demo/mcp",
                    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})");
    ASSERT_EQ(initialized.status, 200);
    ASSERT_FALSE(initialized.session_id.empty());
    const auto notification =
            request(fixture.port(), fiber::http::HttpMethod::Post, "/demo/mcp",
                    R"({"jsonrpc":"2.0","method":"notifications/initialized"})", initialized.session_id);
    ASSERT_EQ(notification.status, 202);

    const auto listed =
            request(fixture.proxy_port(), fiber::http::HttpMethod::Post, "/demo/mcp",
                    R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})", initialized.session_id);
    EXPECT_EQ(listed.error, fiber::common::IoErr::None);
    EXPECT_EQ(listed.status, 200);
    EXPECT_EQ(listed.session_id, initialized.session_id);
    EXPECT_NE(listed.body.find("\"name\":\"weather\""), std::string::npos);
}

} // namespace
