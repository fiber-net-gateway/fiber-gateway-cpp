#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <cerrno>
#include <sys/socket.h>

#include <async/Spawn.h>
#include <async/Yield.h>
#include <common/IoError.h>
#include <event/EventLoop.h>
#include <fiber/nacos/ConfigService.h>
#include <fiber/nacos/NamingService.h>
#include <http/Http1Server.h>
#include <http/HttpBodySpec.h>
#include <http/HttpExchange.h>
#include <http/HttpHeaders.h>
#include <net/SocketAddress.h>

#include "../../../tests/NacosSnapshotTestBuilder.h"
#include "../../../tests/NacosSubscriptionStub.h"
#include "mcp/McpConfigManager.h"
#include "mcp/McpJsonCodec.h"
#include "mcp/McpToolLoader.h"

namespace {

class FakeConfigService final : public fiber::nacos::ConfigService {
public:
    using Result = fiber::nacos::SubscriptionResult<fiber::nacos::ConfigData>;

    fiber::common::IoResult<void> start() noexcept override { return {}; }
    fiber::async::Task<void> shutdown() noexcept override { co_return; }
    fiber::async::Task<std::expected<std::shared_ptr<const fiber::nacos::ConfigData>, fiber::nacos::ConfigServiceError>>
    get_config(std::string, std::string) noexcept override {
        co_return fiber::tests::make_config_data(fiber::nacos::ConfigState::NotFound);
    }
    fiber::async::Task<std::expected<void, fiber::nacos::ConfigServiceError>>
    publish(std::string, std::string, std::string, fiber::nacos::ConfigType,
            std::optional<std::string>) noexcept override {
        co_return std::expected<void, fiber::nacos::ConfigServiceError>{};
    }
    fiber::async::Task<std::expected<void, fiber::nacos::ConfigServiceError>>
    remove_config(std::string, std::string) noexcept override {
        co_return std::expected<void, fiber::nacos::ConfigServiceError>{};
    }
    std::expected<fiber::nacos::Subscription<fiber::nacos::ConfigData>, fiber::nacos::ConfigServiceError>
    subscribe(std::string_view data_id, std::string_view group,
              fiber::nacos::Subscription<fiber::nacos::ConfigData>::NotifyCallback callback, void *context) override {
        return entries_[key(data_id, group)].subscribe(callback, context);
    }

    void push(std::string_view data_id, std::string content) {
        entries_[key(data_id, fiber::ai_server::kMcpConfigGroup)].publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data = fiber::tests::make_config_data(fiber::nacos::ConfigState::Present, "md5", std::move(content)),
        });
    }

private:
    static std::string key(std::string_view data_id, std::string_view group) {
        std::string output(data_id);
        output.push_back('\n');
        output.append(group);
        return output;
    }
    std::map<std::string, fiber::tests::NacosSubscriptionStub<fiber::nacos::ConfigData>, std::less<>> entries_;
};

class FakeNamingService final : public fiber::nacos::NamingService {
public:
    using Result = fiber::nacos::SubscriptionResult<fiber::nacos::ServiceInfo>;

    fiber::common::IoResult<void> start() noexcept override { return {}; }
    fiber::async::Task<void> shutdown() noexcept override { co_return; }
    fiber::async::Task<
            std::expected<std::shared_ptr<const fiber::nacos::ServiceInfo>, fiber::nacos::NamingServiceError>>
    get(std::string, std::string) noexcept override {
        co_return std::unexpected(fiber::nacos::NamingServiceError{});
    }
    std::expected<fiber::nacos::Subscription<fiber::nacos::ServiceInfo>, fiber::nacos::NamingServiceError>
    subscribe(std::string_view, std::string_view,
              fiber::nacos::Subscription<fiber::nacos::ServiceInfo>::NotifyCallback callback, void *context) override {
        return subscription_.subscribe(callback, context);
    }
    std::expected<fiber::nacos::InstanceRegistration, fiber::nacos::NamingServiceError>
    registry(std::string_view, std::string_view, fiber::nacos::Instance) override {
        return std::unexpected(fiber::nacos::NamingServiceError{});
    }

    void push(fiber::tests::ServiceInfoTestData info) {
        subscription_.publish(Result{
                .kind = fiber::nacos::ResultKind::Success,
                .data = fiber::tests::make_service_info(std::move(info)),
        });
    }

private:
    fiber::tests::NacosSubscriptionStub<fiber::nacos::ServiceInfo> subscription_;
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

fiber::async::Task<void> send_admin_tool(fiber::http::HttpExchange &exchange, std::string_view body) noexcept {
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set_view("Content-Type", "application/json");
    auto sent = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::ContentLength(body.size()),
            .end_stream = body.empty(),
    });
    if (sent && !body.empty()) {
        (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
    }
}

fiber::async::Task<void> yield_updates() {
    for (std::size_t i = 0; i < 12; ++i) {
        co_await fiber::async::yield();
    }
}

TEST(McpConfigManagerTest, PublishesAtomicProjectsAndRemovesDeletedProject) {
    const std::filesystem::path cache = std::filesystem::temp_directory_path() / "fiber-mcp-config-manager-test";
    std::error_code file_error;
    std::filesystem::remove_all(cache, file_error);
    std::filesystem::create_directories(cache, file_error);
    ASSERT_FALSE(file_error);
    auto loaded = fiber::ai_server::parse_mcp_admin_tool(
            R"({"id":"echo-script","script":"return $.message;","tool":{"name":"echo","description":"Echo","inputSchema":{"type":"object"}}})",
            "echo-script");
    ASSERT_TRUE(loaded);
    {
        std::ofstream output(cache / "echo-script.dat", std::ios::binary);
        const std::string content = fiber::ai_server::encode_mcp_tool_cache(*loaded);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    fiber::event::EventLoop loop;
    FakeConfigService config;
    FakeNamingService naming;
    fiber::ai_server::McpConfigManager manager(loop, config, naming, cache);
    bool completed = false;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        EXPECT_TRUE(manager.start());
        config.push(fiber::ai_server::kMcpProjectsDataId, R"({"version":1,"data":["demo"]})");
        config.push("ploto.ai.tools.demo", R"({"version":1,"data":["echo-script"]})");
        co_await yield_updates();

        auto first = manager.store().snapshot();
        EXPECT_EQ(first->projects.size(), 1u);
        EXPECT_NE(first->find_project("demo"), nullptr);
        EXPECT_NE(first->find_project("demo")->find_tool("echo"), nullptr);
        const std::uint64_t failed_before = manager.failed_updates();

        config.push("ploto.ai.tools.demo", R"({"version":2,"data":[]})");
        co_await yield_updates();
        EXPECT_EQ(manager.failed_updates(), failed_before + 1);
        EXPECT_EQ(manager.store().snapshot()->find_project("demo"), first->find_project("demo"));

        config.push(fiber::ai_server::kMcpProjectsDataId, R"({"version":2,"data":[]})");
        co_await yield_updates();
        EXPECT_EQ(manager.project_subscription_count(), 0u);
        EXPECT_EQ(manager.store().snapshot()->find_project("demo"), nullptr);
        EXPECT_NE(first->find_project("demo"), nullptr);

        co_await manager.shutdown();
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
    std::filesystem::remove_all(cache, file_error);
}

TEST(McpToolLoaderTest, FallsBackFromCorruptCacheToAdminAndRewritesCache) {
    const std::filesystem::path cache = std::filesystem::temp_directory_path() / "fiber-mcp-tool-loader-test";
    std::error_code file_error;
    std::filesystem::remove_all(cache, file_error);
    std::filesystem::create_directories(cache, file_error);
    ASSERT_FALSE(file_error);
    {
        std::ofstream output(cache / "weather-script.dat", std::ios::binary);
        output << "corrupt cache";
    }

    constexpr std::string_view kPayload =
            R"({"id":"weather-script","script":"return $.city;","tool":{"name":"weather","description":"Weather","inputSchema":{"type":"object"}}})";
    fiber::event::EventLoop loop;
    FakeNamingService naming;
    std::string requested_path;
    fiber::http::Http1Server server(loop, [&](fiber::http::HttpExchange &exchange) {
        requested_path = std::string(exchange.uri().path);
        return send_admin_tool(exchange, kPayload);
    });
    ASSERT_TRUE(server.bind({fiber::net::IpAddress::loopback_v4(), 0}, {}));
    const auto port = bound_port(server.fd());
    ASSERT_TRUE(port);
    fiber::ai_server::McpToolLoader loader(loop, naming, cache);
    bool completed = false;
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        fiber::async::spawn([&]() { return server.serve(); });
        auto started = loader.start();
        EXPECT_TRUE(started);
        if (!started) {
            server.close();
            co_await server.shutdown_and_wait();
            loop.stop();
            co_return;
        }
        fiber::tests::ServiceInfoTestData admin;
        admin.name = "ploto-admin-app";
        admin.group_name = "DEFAULT_GROUP";
        admin.hosts.push_back(fiber::nacos::Instance{
                .instance_id = "admin-1",
                .ip = "127.0.0.1",
                .port = *port,
                .weight = 1.0,
                .healthy = true,
                .enabled = true,
                .cluster_name = "daily1-default",
                .service_name = "ploto-admin-app",
        });
        naming.push(std::move(admin));

        auto tool = co_await loader.load("weather-script");
        EXPECT_TRUE(tool);
        if (tool) {
            EXPECT_EQ((*tool)->descriptor.name, "weather");
        }
        EXPECT_EQ(requested_path, "/ploto-admin-app/aiMcp/getTool/weather-script");

        std::ifstream input(cache / "weather-script.dat", std::ios::binary);
        std::string rewritten((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        auto restored = fiber::ai_server::parse_mcp_tool_cache(rewritten, "weather-script");
        EXPECT_TRUE(restored);
        if (restored) {
            EXPECT_EQ(restored->script, "return $.city;");
        }

        co_await loader.shutdown();
        server.close();
        co_await server.shutdown_and_wait();
        completed = true;
        loop.stop();
    });
    loop.run();
    EXPECT_TRUE(completed);
    std::filesystem::remove_all(cache, file_error);
}

} // namespace
