#ifndef FIBER_AI_SERVER_MCP_CONFIG_SNAPSHOT_H
#define FIBER_AI_SERVER_MCP_CONFIG_SNAPSHOT_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <async/Task.h>
#include <common/NonCopyable.h>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::ai_server {

struct McpToolCallResult {
    std::string text;
    bool has_content = false;
    bool is_error = false;
};

class McpToolHandler : public common::NonCopyable {
public:
    virtual ~McpToolHandler() = default;

    [[nodiscard]] virtual async::Task<McpToolCallResult> invoke(http::HttpExchange &exchange,
                                                                std::string_view arguments_json) const noexcept = 0;
};

struct McpToolDescriptor {
    std::string script_id;
    std::string name;
    std::string description;
    std::string input_schema_json;
    std::string tool_json;
};

struct McpTool {
    McpToolDescriptor descriptor;
    std::shared_ptr<const McpToolHandler> handler;
};

struct McpProjectRuntime {
    std::string name;
    std::vector<std::shared_ptr<const McpTool>> tools;

    [[nodiscard]] const McpTool *find_tool(std::string_view tool_name) const noexcept;
};

struct McpConfigSnapshot {
    std::uint64_t generation = 0;
    std::vector<std::shared_ptr<const McpProjectRuntime>> projects;

    [[nodiscard]] const McpProjectRuntime *find_project(std::string_view project_name) const noexcept;
    [[nodiscard]] std::shared_ptr<const McpProjectRuntime>
    find_project_shared(std::string_view project_name) const noexcept;
};

class McpConfigStore final : public common::NonCopyable {
public:
    McpConfigStore() noexcept;

    void update(std::shared_ptr<const McpConfigSnapshot> snapshot) noexcept;
    [[nodiscard]] std::shared_ptr<const McpConfigSnapshot> snapshot() const noexcept;

private:
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const McpConfigSnapshot>> current_;
#else
    std::shared_ptr<const McpConfigSnapshot> current_;
#endif
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_MCP_CONFIG_SNAPSHOT_H
