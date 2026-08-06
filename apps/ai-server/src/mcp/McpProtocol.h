#ifndef FIBER_AI_SERVER_MCP_PROTOCOL_H
#define FIBER_AI_SERVER_MCP_PROTOCOL_H

#include "McpSessionManager.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <async/Task.h>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::ai_server {

inline constexpr std::string_view kMcpLatestProtocolVersion = "2025-03-26";
inline constexpr std::string_view kMcpOldProtocolVersion = "2024-11-05";

enum class McpProtocolErrorCode : std::uint8_t {
    InvalidJson,
    InvalidRequest,
    AlreadyInitialized,
    NotInitialized,
};

struct McpProtocolError {
    McpProtocolErrorCode code = McpProtocolErrorCode::InvalidRequest;
    int http_status = 400;
    int json_rpc_code = -32600;
    std::string message;
};

struct McpProtocolOutput {
    std::vector<std::string> responses;
    bool has_request = false;
};

class McpProtocol final {
public:
    [[nodiscard]] static async::Task<std::expected<McpProtocolOutput, McpProtocolError>>
    process(http::HttpExchange &exchange, const std::shared_ptr<McpSession> &session, std::string_view body,
            std::string_view protocol_header = {}) noexcept;

    [[nodiscard]] static bool supported_version(std::string_view version) noexcept;
    [[nodiscard]] static std::string tools_list_changed_notification();
    [[nodiscard]] static std::string ping_request(std::uint64_t id);
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_MCP_PROTOCOL_H
