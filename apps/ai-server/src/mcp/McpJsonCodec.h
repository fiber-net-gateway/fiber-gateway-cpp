#ifndef FIBER_AI_SERVER_MCP_JSON_CODEC_H
#define FIBER_AI_SERVER_MCP_JSON_CODEC_H

#include "McpConfigSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <common/json/JsonValue.h>

namespace fiber::mem {
class BufPool;
}

namespace fiber::ai_server {

inline constexpr std::string_view kMcpConfigGroup = "AI-SERVER";
inline constexpr std::string_view kMcpProjectsDataId = "ploto.ai.project.lists";
inline constexpr std::string_view kMcpToolsDataIdPrefix = "ploto.ai.tools.";
inline constexpr std::string_view kMcpAdminServiceName = "ploto-admin-app";
inline constexpr std::string_view kMcpAdminServiceGroup = "DEFAULT_GROUP";

enum class McpJsonErrorCode : std::uint8_t {
    InvalidJson,
    InvalidEnvelope,
    MissingField,
    InvalidField,
    DuplicateValue,
    TooLarge,
};

struct McpJsonError {
    McpJsonErrorCode code = McpJsonErrorCode::InvalidJson;
    std::size_t offset = 0;
    std::string field;
    std::string message;
};

struct McpNameSetConfig {
    std::int32_t version = 0;
    std::vector<std::string> names;
};

struct McpLoadedTool {
    McpToolDescriptor descriptor;
    std::string script;
};

[[nodiscard]] std::expected<McpNameSetConfig, McpJsonError> parse_mcp_name_set_config(std::string_view content,
                                                                                      bool project_names);

[[nodiscard]] std::expected<McpLoadedTool, McpJsonError> parse_mcp_admin_tool(std::string_view content,
                                                                              std::string_view expected_script_id);

[[nodiscard]] std::expected<McpLoadedTool, McpJsonError> parse_mcp_tool_cache(std::string_view content,
                                                                              std::string_view expected_script_id);

[[nodiscard]] std::string encode_mcp_tool_cache(const McpLoadedTool &tool);

[[nodiscard]] bool encode_json_any(const json::JsonAny &value, std::string &output,
                                   std::size_t max_bytes = 4 * 1024 * 1024) noexcept;

[[nodiscard]] bool append_json_string(std::string &output, std::string_view value,
                                      std::size_t max_bytes = 4 * 1024 * 1024) noexcept;

[[nodiscard]] bool valid_mcp_project_name(std::string_view name) noexcept;
[[nodiscard]] bool valid_mcp_script_id(std::string_view name) noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_MCP_JSON_CODEC_H
