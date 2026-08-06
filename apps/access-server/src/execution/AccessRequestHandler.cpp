#include "AccessRequestHandler.h"
#include "../observability/AccessRequestTelemetry.h"

#include "../../../../src/http/HttpBodySpec.h"
#include "../../../../src/http/HttpExchange.h"
#include "../../../../src/http/HttpHeaderHash.h"
#include "../../../../src/http/HttpHeaders.h"

#include <cstdint>
#include <limits>
#include <utility>

namespace fiber::access_server {
namespace {

constexpr std::string_view kTraceId = "unknown-trace-id";
constexpr std::string_view kTraceCluster = "HI-TRACE-CLUSTER";
constexpr std::string_view kOriginHostHeader = "ploto-origin-host";

struct RequestHostContext {
    std::string normalized_host;
    std::string_view effective_host;
    std::string_view origin_host;
    std::string_view cluster;
    bool extracted_cluster = false;
};

struct RequestEvaluationContext {
    AccessRequestScriptAdapter adapter;
    AccessRequestTelemetry &telemetry;
    std::span<const PathVariable> matched_path_variables;
    std::string_view request_context_cluster;
};

bool evaluate_template(void *context, const script::Script &program, std::string_view expression, std::string &output,
                       AccessError &error) noexcept {
    auto &request = *static_cast<RequestEvaluationContext *>(context);
    return request.adapter.evaluate_template &&
           request.adapter.evaluate_template(request.adapter.context, request.telemetry.script_context(),
                                             request.matched_path_variables, request.request_context_cluster, program,
                                             expression, output, error);
}

struct RouteMatch {
    const CompiledRoute *route = nullptr;
    std::span<const PathVariable> path_variables;

    [[nodiscard]] explicit operator bool() const noexcept { return route != nullptr; }
};

class RouteMatchContext {
public:
    RouteMatchContext(const std::vector<CompiledRoute> &routes, std::span<PathVariable> path_variables,
                      RequestEvaluationContext &request) noexcept :
        routes_(routes), path_variables_(path_variables), request_(request) {}

    bool matched(std::uint32_t, std::uint32_t route_index) noexcept {
        const CompiledRoute &route = routes_[route_index];
        if (route.condition_program &&
            (!request_.adapter.evaluate_condition ||
             !request_.adapter.evaluate_condition(request_.adapter.context, request_.telemetry.script_context(),
                                                  path_variables_.first(path_variable_count_),
                                                  request_.request_context_cluster, *route.condition_program))) {
            return false;
        }
        matched_route_ = &route;
        matched_path_variables_ = path_variables_.first(path_variable_count_);
        return true;
    }

    void add_path_var(std::string_view name, std::string_view value) noexcept {
        path_variables_[path_variable_count_++] = PathVariable{
                .name = name,
                .value = value,
        };
    }

    void pop_path_var() noexcept { --path_variable_count_; }

    [[nodiscard]] RouteMatch result() const noexcept {
        return RouteMatch{
                .route = matched_route_,
                .path_variables = matched_path_variables_,
        };
    }

private:
    const std::vector<CompiledRoute> &routes_;
    std::span<PathVariable> path_variables_;
    RequestEvaluationContext &request_;
    const CompiledRoute *matched_route_ = nullptr;
    std::span<const PathVariable> matched_path_variables_;
    std::size_t path_variable_count_ = 0;
};

common::IoResult<RouteMatch> match_route(const ProjectRouteSnapshot &project, http::HttpExchange &exchange,
                                         RequestEvaluationContext &evaluation) noexcept {
    const std::size_t variable_capacity = project.max_path_variable_count();
    PathVariable *variable_data = nullptr;
    if (variable_capacity != 0) {
        variable_data = static_cast<PathVariable *>(
                exchange.pool().alloc(variable_capacity * sizeof(PathVariable), alignof(PathVariable)));
        if (!variable_data) {
            return std::unexpected(common::IoErr::NoMem);
        }
    }

    std::span<PathVariable> path_variables(variable_data, variable_capacity);
    RouteMatchContext context(project.routes(), path_variables, evaluation);
    (void) project.match_route_path(exchange.uri().path, context);
    return context.result();
}

RequestHostContext resolve_request_host(const http::HttpExchange &exchange, bool test_mode) {
    RequestHostContext result;
    result.origin_host = exchange.header("Host");
    result.effective_host = result.origin_host;
    if (!test_mode) {
        return result;
    }

    const std::size_t underscore = result.origin_host.find('_');
    const std::size_t dot =
            underscore == std::string_view::npos ? std::string_view::npos : result.origin_host.find('.', underscore);
    if (underscore > 0 && underscore != std::string_view::npos && dot > underscore && dot != std::string_view::npos) {
        result.normalized_host.reserve(result.origin_host.size() - (dot - underscore));
        result.normalized_host.append(result.origin_host.substr(0, underscore));
        result.normalized_host.append(result.origin_host.substr(dot));
        result.effective_host = result.normalized_host;
        result.cluster = result.origin_host.substr(underscore + 1, dot - underscore - 1);
        result.extracted_cluster = true;
        return result;
    }
    result.cluster = exchange.header(kTraceCluster);
    return result;
}

std::uint8_t entry_bit(std::string_view entry) noexcept {
    if (entry == "vdi") {
        return kNetVdi;
    }
    if (entry == "desktop") {
        return kNetOffice;
    }
    if (entry == "internet") {
        return kNetInternet;
    }
    return 0;
}

bool is_https(const http::HttpExchange &exchange) noexcept {
    constexpr std::string_view kHttps = "https";
    const std::string_view forwarded = exchange.header("X-Forwarded-Proto");
    return forwarded.size() == kHttps.size() && http::http_header_name_equals_ci(forwarded, kHttps);
}

bool cidr_matches_any(std::span<const Cidr> cidrs, const Cidr &target) noexcept {
    for (const Cidr &cidr: cidrs) {
        if (cidr.matches(target)) {
            return true;
        }
    }
    return false;
}

bool source_ip_allowed(const CompiledRoute &route, std::string_view real_ip) {
    if (real_ip.empty()) {
        return true;
    }

    const std::size_t colon = real_ip.rfind(':');
    const std::size_t address_end = colon == std::string_view::npos ? real_ip.size() : colon;
    auto target = Cidr::parse(real_ip.substr(0, address_end), "X-Real-Ip");
    if (!target) {
        // Java skips allow/deny checks when X-Real-Ip cannot be parsed.
        return true;
    }
    if (!route.allow_cidrs.empty() && !cidr_matches_any(route.allow_cidrs, *target)) {
        return false;
    }
    return route.deny_cidrs.empty() || !cidr_matches_any(route.deny_cidrs, *target);
}

std::size_t request_body_limit(const CompiledRoute &route, std::size_t default_limit) noexcept {
    if (!route.max_client_body_size || *route.max_client_body_size == 0) {
        return default_limit;
    }
    if (*route.max_client_body_size < 0) {
        // AbstractRouteExecution clamps an explicit negative value to zero;
        // ReqHandler treats zero as unlimited.
        return 0;
    }
    const auto value = static_cast<std::uint64_t>(*route.max_client_body_size);
    if (value > std::numeric_limits<std::size_t>::max()) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(value);
}

async::Task<common::IoResult<void>> send_redirect(http::HttpExchange &exchange, int status, std::string_view host,
                                                  std::span<const EvaluatedHeader> base_headers,
                                                  std::chrono::milliseconds timeout,
                                                  AccessRequestTelemetry &telemetry) noexcept {
    std::string location = "https://";
    location.append(host);
    location.append(exchange.uri().unparsed_uri);

    http::HttpHeaders headers(exchange.pool());
    for (const EvaluatedHeader &header: base_headers) {
        if (!headers.set(header.name, header.value)) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
    }
    if (!headers.set("Location", location) || !telemetry.inject_response_headers(headers)) {
        co_return std::unexpected(common::IoErr::NoMem);
    }

    co_return co_await exchange.send_header(
            {
                    .kind = http::OutgoingHeaderKind::Final,
                    .status_code = status,
                    .headers = &headers,
                    .body = http::HttpBodySpec::ContentLength(0),
                    .connection_mode = http::ResponseConnectionMode::Auto,
                    .end_stream = true,
            },
            timeout);
}

int redirect_status(HttpsStrategy strategy) noexcept { return static_cast<int>(strategy); }

std::string_view request_trace_id(const AccessRequestTelemetry &telemetry) noexcept {
    if (telemetry.trace_id().empty()) {
        return kTraceId;
    }
    return telemetry.trace_id();
}

} // namespace

AccessRequestHandler::AccessRequestHandler(const RouteConfigStore &config_store,
                                           AccessRequestScriptAdapter script_adapter,
                                           AccessRequestHandlerOptions options,
                                           AccessProxyAdapter proxy_adapter) noexcept :
    config_store_(config_store), script_adapter_(script_adapter), options_(options), proxy_adapter_(proxy_adapter),
    project_base_headers_{
            EvaluatedHeader{
                    .name = "Strict-Transport-Security",
                    .value = "max-age=31536000",
            },
    },
    response_executor_(options.response), error_responder_(ErrorResponderOptions{
                                                  .body_timeout = options.response.body_timeout,
                                                  .write_timeout = options.response.write_timeout,
                                          }) {}

async::Task<void> AccessRequestHandler::handle(http::HttpExchange &exchange,
                                               AccessRequestTelemetry &telemetry) const noexcept {
    auto result = co_await handle_impl(exchange, telemetry);
    if (!result) {
        (void) exchange.abort(result.error());
    }
    co_return;
}

async::Task<common::IoResult<void>>
AccessRequestHandler::handle_impl(http::HttpExchange &exchange, AccessRequestTelemetry &telemetry) const noexcept {
    const std::shared_ptr<const AccessRouteSnapshot> snapshot = config_store_.pin();
    RequestHostContext request_host = resolve_request_host(exchange, options_.test_mode);
    const ProjectHostMatch host_match = snapshot->match_host(request_host.effective_host);
    if (!host_match) {
        co_return co_await error_responder_.send(exchange, AccessError::router_not_found(), {}, {},
                                                 request_trace_id(telemetry), false, &telemetry);
    }
    telemetry.set_project(host_match.project->project(), request_host.effective_host, request_host.cluster);

    const auto &base_headers = project_base_headers_;
    const HostStrategyConfig &strategy = host_match.host->strategy;
    if (strategy.net_mask != 0 && (strategy.net_mask & entry_bit(exchange.header("X-Entry"))) == 0) {
        co_return co_await error_responder_.send(exchange, AccessError::entry_error(), base_headers, {},
                                                 request_trace_id(telemetry), false, &telemetry);
    }

    const bool request_is_https = is_https(exchange);
    if (!strategy.https) {
        if (!request_is_https) {
            co_return co_await error_responder_.send(exchange, AccessError::unknown("invalid HTTPS strategy"),
                                                     base_headers, {}, request_trace_id(telemetry), false, &telemetry);
        }
    } else if (*strategy.https != HttpsStrategy::NotRequired && !request_is_https) {
        co_return co_await send_redirect(exchange, redirect_status(*strategy.https), request_host.effective_host,
                                         base_headers, options_.response.write_timeout, telemetry);
    }

    RequestEvaluationContext evaluation{
            .adapter = script_adapter_,
            .telemetry = telemetry,
            .request_context_cluster = request_host.cluster,
    };
    auto route_match_result = match_route(*host_match.project, exchange, evaluation);
    if (!route_match_result) {
        co_return std::unexpected(route_match_result.error());
    }
    const RouteMatch route_match = *route_match_result;
    if (!route_match) {
        co_return co_await error_responder_.send(exchange, AccessError::url_not_matched(host_match.project->project()),
                                                 base_headers, {}, request_trace_id(telemetry), false, &telemetry);
    }

    const CompiledRoute &route = *route_match.route;
    telemetry.set_route(route);
    const std::size_t body_limit = request_body_limit(route, options_.default_max_request_body_size);
    const http::HttpBodySpec body_spec = exchange.request_body_spec();
    if (body_limit != 0 && body_spec.is_content_length() && body_spec.content_length() > body_limit) {
        co_return co_await error_responder_.send(exchange, AccessError::request_body_too_large(), base_headers, {},
                                                 request_trace_id(telemetry), true, &telemetry);
    }
    if (!source_ip_allowed(route, exchange.header("X-Real-Ip"))) {
        co_return co_await error_responder_.send(exchange, AccessError::source_ip_not_allowed(), base_headers, {},
                                                 request_trace_id(telemetry), false, &telemetry);
    }
    evaluation.matched_path_variables = route_match.path_variables;
    TemplateEvaluator template_evaluator;
    if (script_adapter_.evaluate_template) {
        template_evaluator = TemplateEvaluator{
                .context = &evaluation,
                .evaluate = evaluate_template,
        };
    }

    if (route.type == RouteType::Proxy) {
        if (!proxy_adapter_.execute) {
            co_return co_await error_responder_.send(exchange, AccessError::unknown("proxy executor is not configured"),
                                                     base_headers, {}, request_trace_id(telemetry), false, &telemetry);
        }
        const std::string_view origin_host =
                request_host.extracted_cluster && !route.proxy->proxy_headers.contains(kOriginHostHeader)
                        ? request_host.origin_host
                        : std::string_view{};
        co_return co_await proxy_adapter_.execute(proxy_adapter_.context, exchange, *route.proxy,
                                                  ProxyExecutionInput{
                                                          .project = host_match.project->project(),
                                                          .initial_context_cluster = request_host.cluster,
                                                          .origin_host = origin_host,
                                                          .template_evaluator = template_evaluator,
                                                          .max_request_body_size = body_limit,
                                                  },
                                                  base_headers, &telemetry);
    }

    co_return co_await response_executor_.execute(exchange, route, base_headers, template_evaluator, body_limit,
                                                  request_trace_id(telemetry), &telemetry);
}

} // namespace fiber::access_server
