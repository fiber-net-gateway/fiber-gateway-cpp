#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include <async/Spawn.h>
#include <common/mem/IoBufChain.h>
#include <event/EventLoop.h>
#include <http/HttpExchange.h>

#include "mcp/McpProtocol.h"

namespace {

class EchoTool final : public fiber::ai_server::McpToolHandler {
public:
    fiber::async::Task<fiber::ai_server::McpToolCallResult>
    invoke(fiber::http::HttpExchange &, std::string_view arguments_json) const noexcept override {
        co_return fiber::ai_server::McpToolCallResult{
                .text = std::string(arguments_json),
                .has_content = true,
        };
    }
};

std::shared_ptr<const fiber::ai_server::McpProjectRuntime> make_project(std::string tool_name = "echo") {
    auto tool = std::make_shared<fiber::ai_server::McpTool>();
    tool->descriptor.name = tool_name;
    tool->descriptor.tool_json =
            "{\"name\":\"" + tool_name + "\",\"description\":\"Echo\",\"inputSchema\":{\"type\":\"object\"}}";
    tool->handler = std::make_shared<EchoTool>();
    auto project = std::make_shared<fiber::ai_server::McpProjectRuntime>();
    project->name = "demo";
    project->tools.push_back(std::move(tool));
    return project;
}

TEST(McpSessionManagerTest, UsesJavaCompatibleNodePrefix) {
    fiber::ai_server::McpSessionManager manager("7f0000011f90");
    auto session = manager.create(fiber::ai_server::McpTransport::StreamableHttp, make_project());
    EXPECT_EQ(session->id(), "7f0000011f901");
    EXPECT_EQ(manager.parse_prefix(session->id()), "7f0000011f90");
    EXPECT_TRUE(manager.is_local(session->id()));
    EXPECT_EQ(manager.find(session->id()), session);
    EXPECT_TRUE(manager.erase(session->id()));
    EXPECT_EQ(session->state(), fiber::ai_server::McpSessionState::Closed);
}

TEST(McpSessionManagerTest, ExistingSessionsAdoptUpdatedProjectRuntime) {
    fiber::ai_server::McpSessionManager manager("7f0000011f90");
    auto session = manager.create(fiber::ai_server::McpTransport::LegacySse, make_project());
    auto mailbox = std::make_shared<fiber::ai_server::McpStreamMailbox>();
    ASSERT_TRUE(session->attach_stream(mailbox));
    auto updated = make_project("weather");

    manager.update_project("demo", updated, "tools changed");

    EXPECT_EQ(session->project(), updated);
    EXPECT_NE(session->project()->find_tool("weather"), nullptr);
    const auto messages = mailbox->take();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0], "tools changed");
    manager.close_all();
}

TEST(McpSessionManagerTest, ExpiresIdleSessionsAndBoundsStreamMailbox) {
    using Clock = std::chrono::steady_clock;
    fiber::ai_server::McpSessionManager manager("7f0000011f90");
    const auto start = Clock::time_point{};
    auto idle = manager.create(fiber::ai_server::McpTransport::StreamableHttp, make_project(), start);
    auto streaming = manager.create(fiber::ai_server::McpTransport::StreamableHttp, make_project(), start);
    auto mailbox = std::make_shared<fiber::ai_server::McpStreamMailbox>();
    ASSERT_TRUE(streaming->attach_stream(mailbox));

    EXPECT_EQ(manager.sweep(start + std::chrono::seconds(61)), 1u);
    EXPECT_EQ(idle->state(), fiber::ai_server::McpSessionState::Closed);
    EXPECT_EQ(manager.find(streaming->id()), streaming);

    for (std::size_t i = 0; i < fiber::ai_server::McpStreamMailbox::kMaxMessages; ++i) {
        ASSERT_TRUE(mailbox->push("message"));
    }
    EXPECT_FALSE(mailbox->push("overflow"));
    EXPECT_TRUE(mailbox->closed());
    EXPECT_TRUE(mailbox->take().empty());
    manager.close_all();
}

TEST(McpProtocolTest, InitializesListsAndCallsTools) {
    fiber::event::EventLoop loop;
    fiber::mem::IoBufNodePool nodes;
    fiber::http::HttpExchange exchange(nodes, {}, {});
    fiber::ai_server::McpSessionManager manager("7f0000011f90");
    auto session = manager.create(fiber::ai_server::McpTransport::StreamableHttp, make_project());
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto initialized = co_await fiber::ai_server::McpProtocol::process(
                exchange, session,
                R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})");
        EXPECT_TRUE(initialized);
        EXPECT_EQ(initialized->responses.size(), 1u);
        EXPECT_NE(initialized->responses[0].find("\"protocolVersion\":\"2025-03-26\""), std::string::npos);
        EXPECT_EQ(session->state(), fiber::ai_server::McpSessionState::Negotiated);

        auto notified = co_await fiber::ai_server::McpProtocol::process(
                exchange, session, R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})");
        EXPECT_TRUE(notified);
        EXPECT_FALSE(notified->has_request);
        EXPECT_EQ(session->state(), fiber::ai_server::McpSessionState::Initialized);

        auto listed = co_await fiber::ai_server::McpProtocol::process(
                exchange, session, R"({"jsonrpc":"2.0","id":"tools","method":"tools/list"})");
        EXPECT_TRUE(listed);
        EXPECT_EQ(listed->responses.size(), 1u);
        EXPECT_NE(listed->responses[0].find("\"name\":\"echo\""), std::string::npos);

        auto called = co_await fiber::ai_server::McpProtocol::process(
                exchange, session,
                R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo","arguments":{"message":"hello"}}})");
        EXPECT_TRUE(called);
        EXPECT_EQ(called->responses.size(), 1u);
        EXPECT_NE(called->responses[0].find("{\\\"message\\\":\\\"hello\\\"}"), std::string::npos);
        EXPECT_NE(called->responses[0].find("\"isError\":false"), std::string::npos);

        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

TEST(McpProtocolTest, RejectsBatchInitializationAndUnknownMethods) {
    fiber::event::EventLoop loop;
    fiber::mem::IoBufNodePool nodes;
    fiber::http::HttpExchange exchange(nodes, {}, {});
    fiber::ai_server::McpSessionManager manager("7f0000011f90");
    auto session = manager.create(fiber::ai_server::McpTransport::StreamableHttp, make_project());
    bool completed = false;

    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        auto invalid = co_await fiber::ai_server::McpProtocol::process(
                exchange, session,
                R"([{"jsonrpc":"2.0","id":1,"method":"initialize"},{"jsonrpc":"2.0","method":"ping"}])");
        EXPECT_FALSE(invalid);
        EXPECT_EQ(invalid.error().code, fiber::ai_server::McpProtocolErrorCode::InvalidRequest);

        auto initialized = co_await fiber::ai_server::McpProtocol::process(
                exchange, session, R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
        EXPECT_TRUE(initialized);
        auto unknown = co_await fiber::ai_server::McpProtocol::process(
                exchange, session, R"({"jsonrpc":"2.0","id":2,"method":"resources/list"})");
        EXPECT_TRUE(unknown);
        EXPECT_EQ(unknown->responses.size(), 1u);
        EXPECT_NE(unknown->responses[0].find("\"code\":-32601"), std::string::npos);

        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
}

} // namespace
