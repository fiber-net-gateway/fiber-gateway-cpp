#ifndef FIBER_AI_SERVER_MCP_HTTP_HANDLER_H
#define FIBER_AI_SERVER_MCP_HTTP_HANDLER_H

#include "../mcp/McpConfigSnapshot.h"
#include "../mcp/McpSessionForwarder.h"
#include "../mcp/McpSessionManager.h"

#include <memory>
#include <string_view>

#include <async/Task.h>
#include <common/NonCopyable.h>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::ai_server {

class McpHttpHandler final : public common::NonCopyable {
public:
    McpHttpHandler(McpConfigStore &config, McpSessionManager &sessions,
                   McpSessionForwarder *forwarder = nullptr) noexcept :
        config_(&config), sessions_(&sessions), forwarder_(forwarder) {}

    [[nodiscard]] bool matches(std::string_view path) const noexcept;
    [[nodiscard]] async::Task<void> handle(http::HttpExchange &exchange) noexcept;

private:
    [[nodiscard]] async::Task<void> handle_streamable(http::HttpExchange &exchange, std::string_view project_name,
                                                      std::shared_ptr<const McpProjectRuntime> project) noexcept;
    [[nodiscard]] async::Task<void> handle_legacy_sse(http::HttpExchange &exchange,
                                                      std::shared_ptr<const McpProjectRuntime> project) noexcept;
    [[nodiscard]] async::Task<void> handle_legacy_message(http::HttpExchange &exchange,
                                                          std::string_view project_name) noexcept;
    [[nodiscard]] async::Task<void> stream(http::HttpExchange &exchange, const std::shared_ptr<McpSession> &session,
                                           const std::shared_ptr<McpStreamMailbox> &mailbox,
                                           std::string endpoint = {}) noexcept;

    McpConfigStore *config_ = nullptr;
    McpSessionManager *sessions_ = nullptr;
    McpSessionForwarder *forwarder_ = nullptr;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_MCP_HTTP_HANDLER_H
