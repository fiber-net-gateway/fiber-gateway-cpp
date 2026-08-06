#include "McpConfigSnapshot.h"

namespace fiber::ai_server {

const McpTool *McpProjectRuntime::find_tool(std::string_view tool_name) const noexcept {
    const auto it = std::lower_bound(tools.begin(), tools.end(), tool_name,
                                     [](const std::shared_ptr<const McpTool> &tool, std::string_view name) {
                                         return tool->descriptor.name < name;
                                     });
    return it != tools.end() && (*it)->descriptor.name == tool_name ? it->get() : nullptr;
}

const McpProjectRuntime *McpConfigSnapshot::find_project(std::string_view project_name) const noexcept {
    const auto project = find_project_shared(project_name);
    return project.get();
}

std::shared_ptr<const McpProjectRuntime>
McpConfigSnapshot::find_project_shared(std::string_view project_name) const noexcept {
    const auto it = std::lower_bound(projects.begin(), projects.end(), project_name,
                                     [](const std::shared_ptr<const McpProjectRuntime> &project,
                                        std::string_view name) { return project->name < name; });
    return it != projects.end() && (*it)->name == project_name ? *it : nullptr;
}

McpConfigStore::McpConfigStore() noexcept { update(std::make_shared<const McpConfigSnapshot>()); }

void McpConfigStore::update(std::shared_ptr<const McpConfigSnapshot> snapshot) noexcept {
    if (!snapshot) {
        snapshot = std::make_shared<const McpConfigSnapshot>();
    }
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    current_.store(std::move(snapshot), std::memory_order_release);
#else
    std::atomic_store_explicit(&current_, std::move(snapshot), std::memory_order_release);
#endif
}

std::shared_ptr<const McpConfigSnapshot> McpConfigStore::snapshot() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    return current_.load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(&current_, std::memory_order_acquire);
#endif
}

} // namespace fiber::ai_server
