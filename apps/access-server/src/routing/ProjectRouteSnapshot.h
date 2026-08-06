#ifndef FIBER_ACCESS_SERVER_PROJECT_ROUTE_SNAPSHOT_H
#define FIBER_ACCESS_SERVER_PROJECT_ROUTE_SNAPSHOT_H

#include "../../../../src/common/util/RoutePathMatcher.h"
#include "../config/AccessConfig.h"
#include "../config/AccessConfigError.h"
#include "Cidr.h"
#include "CompiledHeaderTemplates.h"
#include "HostMatcher.h"
#include "ProxyAddressSelector.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <async/Task.h>

namespace fiber::access_server {

enum class ResponseBodyKind : std::uint8_t {
    Empty,
    Text,
    Base64,
    Template,
};

struct CompiledResponseRoute {
    std::int32_t status = 0;
    ResponseBodyKind body_kind = ResponseBodyKind::Empty;
    // TEXT and decoded BASE64 contain response bytes. TEMPLATE uses
    // body_template and leaves this string empty.
    std::string body;
    std::optional<CompiledTemplate> body_template;
    std::vector<CompiledTemplateEntry> response_headers;
};

struct CompiledProxyRoute {
    std::shared_ptr<ProxyAddressSelector> address_selector;
    std::int32_t timeout_millis = 60000;
    std::optional<std::int64_t> max_response_body_size;
    std::optional<std::int32_t> websocket_timeout_millis;
    std::optional<bool> flush;
    CompiledHeaderTemplates proxy_headers;
    CompiledHeaderTemplates response_headers;
    std::vector<CompiledTemplateEntry> context;
    std::optional<CompiledTemplate> rewrite;
};

struct CompiledRoute {
    std::string path;
    std::string key;
    RouteType type = RouteType::Proxy;
    std::optional<script::Script> condition_program;
    std::vector<std::string> path_variable_names;
    std::optional<std::int64_t> max_client_body_size;
    std::vector<Cidr> allow_cidrs;
    std::vector<Cidr> deny_cidrs;
    std::optional<CompiledProxyRoute> proxy;
    std::optional<CompiledResponseRoute> response;
};

struct CompiledHost {
    std::string pattern;
    HostStrategyConfig strategy;
};

struct PathVariable {
    std::string_view name;
    std::string_view value;
};

struct ScriptCompilerAdapter {
    using Result = std::expected<script::Script, std::string>;
    using Function = Result (*)(void *context, std::string_view expression,
                                std::span<const std::string> path_variable_names);

    void *context = nullptr;
    Function compile_expression = nullptr;
};

class ProjectRouteSnapshot {
public:
    [[nodiscard]] std::string_view project() const noexcept { return project_; }
    [[nodiscard]] std::int32_t version() const noexcept { return version_; }
    [[nodiscard]] const std::vector<CompiledHost> &hosts() const noexcept { return hosts_; }
    [[nodiscard]] const std::vector<CompiledRoute> &routes() const noexcept { return routes_; }
    [[nodiscard]] std::size_t max_path_variable_count() const noexcept { return path_matcher_.max_path_var_count(); }

    [[nodiscard]] const CompiledHost *match_host(std::string_view host) const noexcept;
    template<typename Context>
    [[nodiscard]] bool match_route_path(std::string_view path, Context &context) const noexcept {
        return path_matcher_.match_path(path, context);
    }
    [[nodiscard]] async::Task<std::expected<void, ProxyAddressReadyError>> wait_ready() const noexcept;
    [[nodiscard]] bool ready_for_publish() const noexcept;

private:
    friend std::expected<std::optional<ProjectRouteSnapshot>, AccessConfigError>
    compile_project_config(std::string_view project, const ProjectConfig &config, ScriptCompilerAdapter compiler,
                           ProxyAddressSelectorFactory selector_factory);

    std::string project_;
    std::int32_t version_ = 0;
    std::vector<CompiledHost> hosts_;
    std::vector<CompiledRoute> routes_;
    HostMatcher host_matcher_;
    util::RoutePathMatcher<std::uint32_t> path_matcher_;
};

using ProjectSnapshotResult = std::expected<std::optional<ProjectRouteSnapshot>, AccessConfigError>;

// A missing/empty host map is the Java watcher unload signal and returns
// std::nullopt without compiling routes.
[[nodiscard]] ProjectSnapshotResult compile_project_config(std::string_view project, const ProjectConfig &config);
[[nodiscard]] ProjectSnapshotResult compile_project_config(std::string_view project, const ProjectConfig &config,
                                                           ScriptCompilerAdapter compiler);
[[nodiscard]] ProjectSnapshotResult compile_project_config(std::string_view project, const ProjectConfig &config,
                                                           ScriptCompilerAdapter compiler,
                                                           ProxyAddressSelectorFactory selector_factory);

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROJECT_ROUTE_SNAPSHOT_H
