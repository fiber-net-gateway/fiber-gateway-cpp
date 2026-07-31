#include "ProjectRouteSnapshot.h"

#include "../../../../src/common/util/Base64.h"

#include <bit>
#include <charconv>
#include <limits>
#include <utility>

namespace fiber::access_server {
namespace {

AccessConfigError route_error(AccessConfigErrorCode code, std::size_t route_index, std::string_view field,
                              std::string_view message) {
    std::string path = "routes[";
    path.append(std::to_string(route_index));
    path.push_back(']');
    if (!field.empty()) {
        path.push_back('.');
        path.append(field);
    }
    return AccessConfigError{
            .code = code,
            .field = std::move(path),
            .message = std::string(message),
    };
}

AccessConfigError project_error(std::string_view field, std::string_view message) {
    return AccessConfigError{
            .code = AccessConfigErrorCode::InvalidCombination,
            .field = std::string(field),
            .message = std::string(message),
    };
}

bool is_nonempty(const std::optional<std::string> &value) noexcept { return value && !value->empty(); }

std::int32_t java_int32_narrow(std::int64_t value) noexcept {
    const auto bits = static_cast<std::uint32_t>(static_cast<std::uint64_t>(value) & 0xFFFF'FFFFULL);
    return std::bit_cast<std::int32_t>(bits);
}

std::string conditional_route_key(std::string_view path, std::string_view condition) {
    std::uint32_t crc = std::numeric_limits<std::uint32_t>::max();
    for (const unsigned char byte: condition) {
        crc ^= byte;
        for (unsigned i = 0; i < 8; ++i) {
            const std::uint32_t low_bit_mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0x82F63B78U & low_bit_mask);
        }
    }
    crc = ~crc;

    constexpr char kHex[] = "0123456789abcdef";
    std::string key;
    key.reserve(path.size() + 9);
    key.append(path);
    key.push_back('@');
    for (unsigned i = 0; i < 8; ++i) {
        key.push_back(kHex[(crc >> (i * 4U)) & 0xFU]);
    }
    return key;
}

std::optional<std::string_view> validate_path_pattern(std::string_view path) noexcept {
    if (path.empty()) {
        return "path is empty";
    }

    std::size_t segment_start = 0;
    bool wildcard = false;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        const auto ch = i < path.size() ? static_cast<unsigned char>(path[i]) : 0U;
        if ((ch & 0x80U) != 0) {
            return "path must use ASCII bytes";
        }
        if (ch == 0 || ch == '/') {
            if (wildcard && ch != 0) {
                return "wildcard segment must be the last path segment";
            }
            segment_start = i + 1;
            wildcard = false;
        } else if (i == segment_start && ch == '*') {
            wildcard = true;
        }
    }
    return std::nullopt;
}

bool validate_template_structure(std::string_view source) noexcept {
    bool escaping = false;
    bool expression = false;
    std::size_t expression_size = 0;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char ch = source[i];
        if (escaping) {
            if (ch != '\\' && ch != '$' && ch != '{' && ch != '}') {
                return false;
            }
            escaping = false;
        } else if (expression) {
            if (ch == '}') {
                if (expression_size == 0) {
                    return false;
                }
                expression = false;
                expression_size = 0;
            } else {
                ++expression_size;
            }
        } else if (ch == '\\') {
            escaping = true;
        } else if (ch == '$' && i + 1 < source.size() && source[i + 1] == '{') {
            ++i;
            expression = true;
            expression_size = 0;
        }
    }
    return !escaping && !expression;
}

std::expected<std::vector<CompiledScriptProgram>, std::string>
compile_template_expressions(std::string_view source, std::span<const std::string> path_variable_names,
                             ScriptCompilerAdapter compiler) {
    std::vector<CompiledScriptProgram> programs;
    if (!compiler.compile_expression) {
        return programs;
    }

    std::string expression;
    bool escaping = false;
    bool in_expression = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char ch = source[i];
        if (escaping) {
            if (in_expression) {
                expression.push_back(ch);
            }
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (in_expression) {
            if (ch == '}') {
                auto program = compiler.compile_expression(compiler.context, expression, path_variable_names);
                if (!program) {
                    return std::unexpected(std::move(program.error()));
                }
                programs.push_back(std::move(*program));
                expression.clear();
                in_expression = false;
            } else {
                expression.push_back(ch);
            }
            continue;
        }
        if (ch == '$' && i + 1 < source.size() && source[i + 1] == '{') {
            ++i;
            in_expression = true;
        }
    }
    return programs;
}

std::expected<std::vector<CompiledTemplateEntry>, AccessConfigError>
compile_templates(const StringConfigMap &input, std::size_t route_index, std::string_view field) {
    std::vector<CompiledTemplateEntry> result;
    result.reserve(input.size());
    for (const StringConfigEntry &entry: input) {
        if (!entry.value || !validate_template_structure(*entry.value)) {
            return std::unexpected(
                    route_error(AccessConfigErrorCode::InvalidField, route_index, field, "invalid template"));
        }
        result.push_back(CompiledTemplateEntry{
                .name = entry.name,
                .source = *entry.value,
        });
    }
    return result;
}

std::expected<void, AccessConfigError> compile_route_scripts(CompiledRoute &route, std::size_t route_index,
                                                             ScriptCompilerAdapter compiler) {
    if (!compiler.compile_expression) {
        return {};
    }

    auto compile_template =
            [&](std::string_view source, std::string_view field,
                std::vector<CompiledScriptProgram> &programs) -> std::expected<void, AccessConfigError> {
        auto compiled = compile_template_expressions(source, route.path_variable_names, compiler);
        if (!compiled) {
            return std::unexpected(
                    route_error(AccessConfigErrorCode::InvalidField, route_index, field, compiled.error()));
        }
        programs = std::move(*compiled);
        return {};
    };
    auto compile_entries = [&](std::vector<CompiledTemplateEntry> &entries,
                               std::string_view field) -> std::expected<void, AccessConfigError> {
        for (CompiledTemplateEntry &entry: entries) {
            auto compiled = compile_template(entry.source, field, entry.expression_programs);
            if (!compiled) {
                return compiled;
            }
        }
        return {};
    };

    if (route.condition) {
        auto program = compiler.compile_expression(compiler.context, *route.condition, route.path_variable_names);
        if (!program) {
            return std::unexpected(
                    route_error(AccessConfigErrorCode::InvalidField, route_index, "condition", program.error()));
        }
        route.condition_program = std::move(*program);
    }

    if (route.response) {
        if (route.response->body_kind == ResponseBodyKind::Template) {
            auto compiled = compile_template(route.response->body, "body", route.response->body_expression_programs);
            if (!compiled) {
                return compiled;
            }
        }
        return compile_entries(route.response->response_headers, "response_headers");
    }

    auto proxy_headers = compile_entries(route.proxy->proxy_headers, "proxy_headers");
    if (!proxy_headers) {
        return proxy_headers;
    }
    auto response_headers = compile_entries(route.proxy->response_headers, "response_headers");
    if (!response_headers) {
        return response_headers;
    }
    auto context = compile_entries(route.proxy->context, "context");
    if (!context) {
        return context;
    }
    if (route.proxy->rewrite) {
        return compile_template(*route.proxy->rewrite, "rewrite", route.proxy->rewrite_expression_programs);
    }
    return {};
}

bool ascii_iequals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto left_byte = static_cast<unsigned char>(left[i]);
        const auto right_byte = static_cast<unsigned char>(right[i]);
        const auto left_fold = left_byte >= 'A' && left_byte <= 'Z' ? left_byte | 0x20U : left_byte;
        const auto right_fold = right_byte >= 'A' && right_byte <= 'Z' ? right_byte | 0x20U : right_byte;
        if (left_fold != right_fold) {
            return false;
        }
    }
    return true;
}

std::expected<std::vector<CompiledTemplateEntry>, AccessConfigError> compile_context(const StringConfigMap &input,
                                                                                     std::size_t route_index) {
    auto compiled = compile_templates(input, route_index, "context");
    if (!compiled) {
        return compiled;
    }

    constexpr std::string_view kTraceCluster = "HI-TRACE-CLUSTER";
    std::vector<CompiledTemplateEntry> result;
    result.reserve(compiled->size());
    for (CompiledTemplateEntry &entry: *compiled) {
        if (ascii_iequals(entry.name, "cluster") || ascii_iequals(entry.name, kTraceCluster)) {
            entry.name = kTraceCluster;
        }

        bool replaced = false;
        for (CompiledTemplateEntry &existing: result) {
            if (existing.name == entry.name) {
                existing.source = std::move(entry.source);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            result.push_back(std::move(entry));
        }
    }
    return result;
}

std::optional<std::int32_t> parse_java_port(std::string_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    bool positive_sign = false;
    if (value.front() == '+') {
        positive_sign = true;
        value.remove_prefix(1);
        if (value.empty()) {
            return std::nullopt;
        }
    }
    std::int32_t port = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || (positive_sign && port < 0)) {
        return std::nullopt;
    }
    return port;
}

std::optional<CompiledProxyAddress> compile_java_http_host(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    std::string_view host = value;
    std::string_view scheme_name;
    const std::size_t scheme_offset = host.find("://");
    if (scheme_offset != std::string_view::npos && scheme_offset > 0) {
        scheme_name = host.substr(0, scheme_offset);
        host.remove_prefix(scheme_offset + 3);
    }

    std::int32_t configured_port = -1;
    const std::size_t colon = host.rfind(':');
    if (colon > 0 && colon != std::string_view::npos) {
        const auto port = parse_java_port(host.substr(colon + 1));
        if (!port) {
            return std::nullopt;
        }
        configured_port = *port;
        host = host.substr(0, colon);
    }
    if (host.empty()) {
        return std::nullopt;
    }

    const bool https = scheme_name.empty() ? configured_port == 443 : ascii_iequals(scheme_name, "https");
    const std::uint16_t default_port = https ? 443 : 80;
    const std::int64_t real_port = configured_port <= 0 ? default_port : configured_port;
    if (real_port > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
    }

    CompiledProxyAddress address{
            .scheme = https ? ProxyUpstreamScheme::Https : ProxyUpstreamScheme::Http,
            .host = std::string(host),
            .port = static_cast<std::uint16_t>(real_port),
            .host_header = std::string(host),
    };
    if (address.port != default_port) {
        address.host_header.push_back(':');
        address.host_header.append(std::to_string(address.port));
    }
    net::IpAddress ip;
    if (net::IpAddress::parse(host, ip)) {
        address.ip_address = ip;
    }
    return address;
}

std::expected<std::vector<Cidr>, AccessConfigError> compile_cidr_list(const std::vector<std::string_view> &items,
                                                                      std::size_t route_index) {
    auto result = Cidr::parse_list(items, "allows");
    if (!result) {
        AccessConfigError error = std::move(result.error());
        error.field = "routes[" + std::to_string(route_index) + "].allows";
        return std::unexpected(std::move(error));
    }
    return result;
}

std::expected<CompiledRoute, AccessConfigError> compile_route(const RouteConfig &source, std::size_t route_index) {
    if (!source.path || source.path->empty()) {
        return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "path", "path is empty"));
    }
    if (const auto invalid = validate_path_pattern(*source.path)) {
        return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "path", *invalid));
    }
    if (!source.type) {
        return std::unexpected(
                route_error(AccessConfigErrorCode::InvalidField, route_index, "type", "route type is null"));
    }

    CompiledRoute route;
    route.path = *source.path;
    route.type = *source.type;
    route.max_client_body_size = source.max_client_body_size;
    if (is_nonempty(source.condition)) {
        route.condition = *source.condition;
        route.key = conditional_route_key(route.path, *route.condition);
    } else {
        route.key = route.path;
    }

    if (route.type == RouteType::Response) {
        if (source.status < 100 || source.status >= 1000) {
            return std::unexpected(
                    route_error(AccessConfigErrorCode::OutOfRange, route_index, "status", "invalid status code"));
        }

        CompiledResponseRoute response;
        response.status = source.status;
        if (source.body) {
            const RouteBodyConfig &body = *source.body;
            if (body.type == BodyType::Base64) {
                if (!body.content || !util::base64_decode(*body.content, response.body)) {
                    return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "body",
                                                       "invalid base64 response body"));
                }
                response.body_kind = ResponseBodyKind::Base64;
            } else if (body.type == BodyType::Text) {
                if (!body.content || body.content->empty()) {
                    return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "body",
                                                       "text body content is empty"));
                }
                response.body_kind = ResponseBodyKind::Text;
                response.body = *body.content;
            } else {
                if (!body.content || !validate_template_structure(*body.content)) {
                    return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "body",
                                                       "invalid template response body"));
                }
                response.body_kind = ResponseBodyKind::Template;
                response.body = *body.content;
            }
        }

        auto headers = compile_templates(source.response_headers, route_index, "response_headers");
        if (!headers) {
            return std::unexpected(std::move(headers.error()));
        }
        response.response_headers = std::move(*headers);
        route.response.emplace(std::move(response));
    } else {
        if (source.timeout_millis && *source.timeout_millis < 5) {
            return std::unexpected(
                    route_error(AccessConfigErrorCode::OutOfRange, route_index, "timeout", "timeout is too small"));
        }

        CompiledProxyRoute proxy;
        proxy.timeout_millis = source.timeout_millis ? java_int32_narrow(*source.timeout_millis) : 60000;
        if (is_nonempty(source.service)) {
            proxy.upstream_kind = ProxyUpstreamKind::Service;
            const std::size_t slash = source.service->find('/');
            if (slash > 0 && slash != std::string::npos) {
                proxy.service = source.service->substr(0, slash);
                proxy.cluster = source.service->substr(slash + 1);
            } else {
                proxy.service = *source.service;
            }
            if (is_nonempty(source.cluster)) {
                proxy.cluster = *source.cluster;
            }
        } else if (!source.addresses.empty()) {
            proxy.upstream_kind = ProxyUpstreamKind::Addresses;
            proxy.addresses.reserve(source.addresses.size());
            for (const std::optional<std::string> &address: source.addresses) {
                auto compiled_address = address ? compile_java_http_host(*address) : std::nullopt;
                if (!compiled_address) {
                    return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, route_index, "addresses",
                                                       "invalid HTTP host"));
                }
                proxy.addresses.push_back(std::move(*compiled_address));
            }
        } else {
            return std::unexpected(route_error(AccessConfigErrorCode::InvalidCombination, route_index, "service",
                                               "no service or addresses"));
        }

        auto proxy_headers = compile_templates(source.proxy_headers, route_index, "proxy_headers");
        if (!proxy_headers) {
            return std::unexpected(std::move(proxy_headers.error()));
        }
        proxy.proxy_headers = std::move(*proxy_headers);

        auto response_headers = compile_templates(source.response_headers, route_index, "response_headers");
        if (!response_headers) {
            return std::unexpected(std::move(response_headers.error()));
        }
        proxy.response_headers = std::move(*response_headers);

        auto context = compile_context(source.context, route_index);
        if (!context) {
            return std::unexpected(std::move(context.error()));
        }
        proxy.context = std::move(*context);

        if (is_nonempty(source.rewrite)) {
            if (!validate_template_structure(*source.rewrite)) {
                return std::unexpected(
                        route_error(AccessConfigErrorCode::InvalidField, route_index, "rewrite", "invalid template"));
            }
            proxy.rewrite = *source.rewrite;
        }
        proxy.max_response_body_size = source.max_proxy_body_size;
        if (source.websocket_timeout_millis && *source.websocket_timeout_millis > 0) {
            proxy.websocket_timeout_millis = java_int32_narrow(*source.websocket_timeout_millis);
        }
        proxy.flush = source.flush;
        route.proxy.emplace(std::move(proxy));
    }

    std::vector<std::string_view> allows;
    std::vector<std::string_view> denies;
    allows.reserve(source.allows.size());
    denies.reserve(source.allows.size());
    for (const std::optional<std::string> &item: source.allows) {
        if (!item || item->empty()) {
            return std::unexpected(
                    route_error(AccessConfigErrorCode::InvalidField, route_index, "allows", "empty cidr"));
        }
        if (item->front() == '!') {
            denies.push_back(std::string_view(*item).substr(1));
        } else {
            allows.push_back(*item);
        }
    }
    if (!allows.empty()) {
        auto parsed = compile_cidr_list(allows, route_index);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        route.allow_cidrs = std::move(*parsed);
    }
    if (!denies.empty()) {
        auto parsed = compile_cidr_list(denies, route_index);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        route.deny_cidrs = std::move(*parsed);
    }
    return route;
}

class RouteDefiner {
public:
    explicit RouteDefiner(std::vector<CompiledRoute> &routes) : routes_(routes) {}

    void add_path_var_definer(std::uint32_t &route_index, std::string_view name, std::uint32_t index) {
        CompiledRoute &route = routes_[route_index];
        for (std::size_t i = 0; i < route.path_variable_names.size(); ++i) {
            if (route.path_variable_names[i] == name && i != index) {
                set_error(route_index, AccessConfigErrorCode::Conflict, "duplicated path variable");
                return;
            }
        }
        route.path_variable_names.emplace_back(name);
    }

    std::uint32_t on_route_mount(std::uint32_t node_id, std::string_view, std::uint32_t &route_index) {
        if (last_node_id_ == node_id && last_route_ != kNoRoute && !routes_[last_route_].condition) {
            set_error(route_index, AccessConfigErrorCode::Conflict, "exists dead route");
        }
        last_node_id_ = node_id;
        last_route_ = route_index;
        return route_index;
    }

    [[nodiscard]] const std::optional<AccessConfigError> &error() const noexcept { return error_; }

private:
    void set_error(std::uint32_t route_index, AccessConfigErrorCode code, std::string_view message) {
        if (!error_) {
            error_ = route_error(code, route_index, "path", message);
        }
    }

    static constexpr std::uint32_t kNoRoute = std::numeric_limits<std::uint32_t>::max();

    std::vector<CompiledRoute> &routes_;
    std::optional<AccessConfigError> error_;
    std::uint32_t last_node_id_ = util::RoutePathMatcher<std::uint32_t>::kInvalidIndex;
    std::uint32_t last_route_ = kNoRoute;
};

class RouteMatchContext {
public:
    RouteMatchContext(const std::vector<CompiledRoute> &routes, std::span<PathVariable> variables,
                      ConditionEvaluator evaluator) noexcept :
        routes_(routes), variables_(variables), evaluator_(evaluator) {}

    bool matched(std::uint32_t, std::uint32_t route_index) noexcept {
        const CompiledRoute &route = routes_[route_index];
        if (route.condition &&
            (!evaluator_.evaluate || !evaluator_.evaluate(evaluator_.context, route.condition_program.get(),
                                                          *route.condition, variables_.first(variable_count_)))) {
            return false;
        }
        matched_route_ = &route;
        matched_variable_count_ = variable_count_;
        return true;
    }

    void add_path_var(std::string_view name, std::string_view value) noexcept {
        variables_[variable_count_++] = PathVariable{
                .name = name,
                .value = value,
        };
    }

    void pop_path_var() noexcept { --variable_count_; }

    [[nodiscard]] const CompiledRoute *route() const noexcept { return matched_route_; }
    [[nodiscard]] std::size_t variable_count() const noexcept { return matched_variable_count_; }

private:
    const std::vector<CompiledRoute> &routes_;
    std::span<PathVariable> variables_;
    ConditionEvaluator evaluator_;
    const CompiledRoute *matched_route_ = nullptr;
    std::size_t variable_count_ = 0;
    std::size_t matched_variable_count_ = 0;
};

} // namespace

const CompiledHost *ProjectRouteSnapshot::match_host(std::string_view host) const noexcept {
    const std::optional<std::uint32_t> index = host_matcher_.match(host);
    if (!index) {
        return nullptr;
    }
    return &hosts_[*index];
}

RouteMatch ProjectRouteSnapshot::match_route(std::string_view path, std::span<PathVariable> path_variables,
                                             ConditionEvaluator evaluator) const noexcept {
    if (path_variables.size() < path_matcher_.max_path_var_count()) {
        return RouteMatch{.insufficient_variable_capacity = true};
    }

    RouteMatchContext context(routes_, path_variables, evaluator);
    (void) path_matcher_.match_path(path, context);
    return RouteMatch{
            .route = context.route(),
            .path_variable_count = context.variable_count(),
    };
}

ProjectSnapshotResult compile_project_config(std::string_view project, const ProjectConfig &config) {
    return compile_project_config(project, config, {});
}

ProjectSnapshotResult compile_project_config(std::string_view project, const ProjectConfig &config,
                                             ScriptCompilerAdapter compiler) {
    if (project.empty()) {
        return std::unexpected(project_error("project", "project name is empty"));
    }
    if (!config.hosts || config.hosts->empty()) {
        return std::optional<ProjectRouteSnapshot>{};
    }
    if (!config.routes) {
        return std::unexpected(project_error("routes", "routes is null while host is configured"));
    }

    ProjectRouteSnapshot snapshot;
    snapshot.project_ = project;
    snapshot.version_ = config.version;
    snapshot.routes_.reserve(config.routes->size());
    for (std::size_t i = 0; i < config.routes->size(); ++i) {
        const std::optional<RouteConfig> &source = (*config.routes)[i];
        if (!source) {
            return std::unexpected(route_error(AccessConfigErrorCode::InvalidField, i, {}, "route entry is null"));
        }
        auto route = compile_route(*source, i);
        if (!route) {
            return std::unexpected(std::move(route.error()));
        }
        snapshot.routes_.push_back(std::move(*route));
    }

    RouteDefiner definer(snapshot.routes_);
    util::RoutePathMatcher<std::uint32_t>::Builder<std::uint32_t, RouteDefiner> path_builder(definer);
    for (std::uint32_t i = 0; i < snapshot.routes_.size(); ++i) {
        path_builder.add_route(snapshot.routes_[i].path, i);
    }
    snapshot.path_matcher_ = path_builder.build();
    if (definer.error()) {
        return std::unexpected(*definer.error());
    }
    for (std::size_t i = 0; i < snapshot.routes_.size(); ++i) {
        auto compiled = compile_route_scripts(snapshot.routes_[i], i, compiler);
        if (!compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
    }

    std::vector<HostPattern> patterns;
    patterns.reserve(config.hosts->size());
    snapshot.hosts_.reserve(config.hosts->size());
    for (const HostConfigEntry &host: *config.hosts) {
        if (host.pattern.empty()) {
            continue;
        }
        if (!host.strategy) {
            return std::unexpected(project_error("host." + host.pattern, "host strategy is null"));
        }
        const std::uint32_t index = static_cast<std::uint32_t>(snapshot.hosts_.size());
        snapshot.hosts_.push_back(CompiledHost{
                .pattern = host.pattern,
                .strategy = *host.strategy,
        });
        patterns.push_back(HostPattern{
                .pattern = snapshot.hosts_.back().pattern,
                .handler = index,
        });
    }

    auto host_matcher = HostMatcher::build(patterns);
    if (!host_matcher) {
        return std::unexpected(std::move(host_matcher.error()));
    }
    snapshot.host_matcher_ = std::move(*host_matcher);
    return std::optional<ProjectRouteSnapshot>(std::move(snapshot));
}

} // namespace fiber::access_server
