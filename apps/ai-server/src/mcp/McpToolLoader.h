#ifndef FIBER_AI_SERVER_MCP_TOOL_LOADER_H
#define FIBER_AI_SERVER_MCP_TOOL_LOADER_H

#include "McpConfigSnapshot.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <async/Task.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <event/EventLoop.h>
#include <fiber/nacos/NamingService.h>
#include <fiber/nacos/Subscription.h>
#include <http/Http1ConnectionPoolCore.h>
#include <net/IpAddress.h>

namespace fiber::http_script {
class HttpScriptServices;
}

namespace fiber::ai_server {

enum class McpToolLoadErrorCode : std::uint8_t {
    InvalidId,
    NoAdminInstance,
    PoolUnavailable,
    Connect,
    Send,
    Receive,
    HttpStatus,
    ResponseTooLarge,
    InvalidTool,
    Compile,
};

struct McpToolLoadError {
    McpToolLoadErrorCode code = McpToolLoadErrorCode::InvalidTool;
    common::IoErr io_error = common::IoErr::None;
    int http_status = 0;
    std::string message;
};

class McpToolLoader final : public common::NonCopyable, public common::NonMovable {
public:
    struct AdminNode {
        net::IpAddress ip;
        std::string host;
        std::uint16_t port = 0;
    };

    McpToolLoader(event::EventLoop &loop, nacos::NamingService &naming_service,
                  std::filesystem::path cache_directory = "cache/ai",
                  http_script::HttpScriptServices *script_services = nullptr);
    ~McpToolLoader();

    [[nodiscard]] std::expected<void, nacos::NamingServiceError> start();
    void stop() noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;
    [[nodiscard]] async::Task<std::expected<std::shared_ptr<const McpTool>, McpToolLoadError>>
    load(std::string script_id) noexcept;

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] std::size_t admin_node_count() const noexcept { return admin_nodes_.size(); }

private:
    static void admin_notify(void *context, const nacos::SubscriptionResult<nacos::ServiceInfo> &result) noexcept;
    void apply_admin(const nacos::ServiceInfo &info);
    [[nodiscard]] std::optional<AdminNode> select_admin();
    [[nodiscard]] async::Task<std::expected<std::string, McpToolLoadError>> fetch_admin(std::string script_id) noexcept;

    event::EventLoop *loop_ = nullptr;
    nacos::NamingService *naming_service_ = nullptr;
    std::filesystem::path cache_directory_;
    http_script::HttpScriptServices *script_services_ = nullptr;
    http::Http1ConnectionPoolCore pool_;
    std::optional<nacos::Subscription<nacos::ServiceInfo>> admin_subscription_;
    std::shared_ptr<const nacos::ServiceInfo> pending_admin_;
    std::vector<AdminNode> admin_nodes_;
    std::size_t next_admin_ = 0;
    bool pool_initialized_ = false;
    bool started_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_MCP_TOOL_LOADER_H
