#ifndef FIBER_AI_SERVER_MCP_SCRIPT_RUNTIME_H
#define FIBER_AI_SERVER_MCP_SCRIPT_RUNTIME_H

#include "McpConfigSnapshot.h"

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace fiber::http_script {
class HttpScriptServices;
}

namespace fiber::ai_server {

struct McpScriptCompileError {
    std::size_t offset = 0;
    std::string message;
};

[[nodiscard]] std::expected<std::shared_ptr<const McpToolHandler>, McpScriptCompileError>
compile_mcp_tool_script(std::string_view source, http_script::HttpScriptServices *services = nullptr);

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_MCP_SCRIPT_RUNTIME_H
