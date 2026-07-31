#ifndef FIBER_ACCESS_SERVER_PROJECT_ROUTE_SNAPSHOT_H
#define FIBER_ACCESS_SERVER_PROJECT_ROUTE_SNAPSHOT_H

#include "../../../../src/common/util/RoutePathMatcher.h"
#include "../config/AccessConfig.h"
#include "../config/AccessConfigError.h"
#include "Cidr.h"
#include "HostMatcher.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fiber::access_server {

using CompiledScriptProgram = std::shared_ptr<const void>;

struct CompiledTemplateEntry {
    std::string name;
    std::string source;
    std::vector<CompiledScriptProgram> expression_programs;
};

enum class ResponseBodyKind : std::uint8_t {
    Empty,
    Text,
    Base64,
    Template,
};

struct CompiledResponseRoute {
    std::int32_t status = 0;
    ResponseBodyKind body_kind = ResponseBodyKind::Empty;
    // TEXT and decoded BASE64 contain response bytes. TEMPLATE contains the
    // original template source for the local script adapter.
    std::string body;
    std::vector<CompiledScriptProgram> body_expression_programs;
    std::vector<CompiledTemplateEntry> response_headers;
};

enum class ProxyUpstreamKind : std::uint8_t {
    Service,
    Addresses,
};

enum class ProxyUpstreamScheme : std::uint8_t {
    Http,
    Https,
};

struct CompiledProxyAddress {
    ProxyUpstreamScheme scheme = ProxyUpstreamScheme::Http;
    std::string host;
    std::uint16_t port = 80;
    std::string host_header;
    std::optional<net::IpAddress> ip_address;
};

struct CompiledProxyRoute {
    ProxyUpstreamKind upstream_kind = ProxyUpstreamKind::Service;
    std::string service;
    std::optional<std::string> cluster;
    std::vector<CompiledProxyAddress> addresses;
    std::int32_t timeout_millis = 60000;
    std::optional<std::int64_t> max_response_body_size;
    std::optional<std::int32_t> websocket_timeout_millis;
    std::optional<bool> flush;
    std::vector<CompiledTemplateEntry> proxy_headers;
    std::vector<CompiledTemplateEntry> response_headers;
    std::vector<CompiledTemplateEntry> context;
    std::optional<std::string> rewrite;
    std::vector<CompiledScriptProgram> rewrite_expression_programs;
};

struct CompiledRoute {
    std::string path;
    std::string key;
    RouteType type = RouteType::Proxy;
    std::optional<std::string> condition;
    CompiledScriptProgram condition_program;
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

struct ConditionEvaluator {
    using Function = bool (*)(void *context, const void *program, std::string_view expression,
                              std::span<const PathVariable> path_variables) noexcept;

    void *context = nullptr;
    Function evaluate = nullptr;
};

struct ScriptCompilerAdapter {
    using Result = std::expected<CompiledScriptProgram, std::string>;
    using Function = Result (*)(void *context, std::string_view expression,
                                std::span<const std::string> path_variable_names);

    void *context = nullptr;
    Function compile_expression = nullptr;
};

struct RouteMatch {
    const CompiledRoute *route = nullptr;
    std::size_t path_variable_count = 0;
    bool insufficient_variable_capacity = false;

    [[nodiscard]] explicit operator bool() const noexcept { return route != nullptr; }
};

class ProjectRouteSnapshot {
public:
    [[nodiscard]] std::string_view project() const noexcept { return project_; }
    [[nodiscard]] std::int32_t version() const noexcept { return version_; }
    [[nodiscard]] const std::vector<CompiledHost> &hosts() const noexcept { return hosts_; }
    [[nodiscard]] const std::vector<CompiledRoute> &routes() const noexcept { return routes_; }
    [[nodiscard]] std::size_t max_path_variable_count() const noexcept { return path_matcher_.max_path_var_count(); }

    [[nodiscard]] const CompiledHost *match_host(std::string_view host) const noexcept;
    [[nodiscard]] RouteMatch match_route(std::string_view path, std::span<PathVariable> path_variables,
                                         ConditionEvaluator evaluator = {}) const noexcept;

private:
    friend std::expected<std::optional<ProjectRouteSnapshot>, AccessConfigError>
    compile_project_config(std::string_view project, const ProjectConfig &config, ScriptCompilerAdapter compiler);

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

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROJECT_ROUTE_SNAPSHOT_H
