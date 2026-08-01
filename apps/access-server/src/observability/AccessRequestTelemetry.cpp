#include "AccessRequestTelemetry.h"
#include "AccessServerLogCategories.h"

#include "../execution/AccessError.h"
#include "../execution/ProxyRequestSender.h"
#include "../routing/ProjectRouteSnapshot.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <string_view>

#include <common/IoError.h>
#include <event/EventLoop.h>
#include <fiber/cat/CatClient.h>
#include <fiber/cat/Status.h>
#include <http/HttpExchange.h>
#include <http/HttpHeaderHash.h>
#include <http/HttpHeaders.h>
#include <log/Log.h>

namespace fiber::access_server {
namespace {

DEFINE_LOGGER(LOG_ACCESS, kAccessServerAccessLogger);

constexpr std::string_view kTraceIdHeader = "Hi-Trace-Id";
constexpr std::string_view kTraceIdLowcaseHeader = "hi-trace-id";
constexpr std::uint64_t kTraceIdHeaderHash = http::http_header_name_hash(kTraceIdLowcaseHeader);
constexpr std::string_view kParentSpanIdHeader = "HI-SPAN-ID-PARENT";
constexpr std::string_view kParentSpanIdLowcaseHeader = "hi-span-id-parent";
constexpr std::uint64_t kParentSpanIdHeaderHash = http::http_header_name_hash(kParentSpanIdLowcaseHeader);
constexpr std::string_view kSpanIdHeader = "HI-SPAN-ID";
constexpr std::string_view kSpanIdLowcaseHeader = "hi-span-id";
constexpr std::uint64_t kSpanIdHeaderHash = http::http_header_name_hash(kSpanIdLowcaseHeader);
constexpr std::size_t kMaxUserAgentBytes = 1024;

bool has_inbound_context(const cat::MessageTraceContext &context) noexcept {
    return !context.message_id.empty() || !context.root_message_id.empty() || !context.parent_message_id.empty();
}

cat::MessageTraceContext read_trace_context(const http::HttpHeaders &headers) noexcept {
    return {
            .message_id = headers.get(kSpanIdLowcaseHeader, kSpanIdHeaderHash),
            .root_message_id = headers.get(kTraceIdLowcaseHeader, kTraceIdHeaderHash),
            .parent_message_id = headers.get(kParentSpanIdLowcaseHeader, kParentSpanIdHeaderHash),
    };
}

std::string_view response_result(const http::HttpResponseStats &response) noexcept {
    if (response.terminal_error != common::IoErr::None || !response.completed) {
        return "canceled";
    }
    if (response.status_code >= 200 && response.status_code < 400) {
        return "success";
    }
    if (response.status_code >= 400 && response.status_code < 500) {
        return "client_error";
    }
    return "server_error";
}

} // namespace

AccessRequestTelemetry::AccessRequestTelemetry(http::HttpExchange &exchange, AccessServerMetrics::Worker *metrics,
                                               cat::CatClient *cat_client) noexcept :
    exchange_(&exchange), metrics_(metrics), started_(event::EventLoop::current().now()), cat_client_(cat_client) {
    if (metrics_) {
        metrics_->request_started();
    }
    if (!cat_client_) {
        return;
    }

    const cat::MessageTraceContext inbound = read_trace_context(exchange.request_headers());
    const bool inherited = has_inbound_context(inbound);
    bool invalid_fallback = false;
    auto created =
            cat_client_->create_isolated_transaction(exchange.pool(), "URL", exchange.uri().path, {.context = inbound});
    if (!created && inherited &&
        (created.error() == cat::RecordError::InvalidContext || created.error() == cat::RecordError::LimitExceeded)) {
        invalid_fallback = true;
        created = cat_client_->create_isolated_transaction(exchange.pool(), "URL", exchange.uri().path);
    }
    if (!created) {
        return;
    }
    root_.emplace(std::move(*created));

    auto propagation = root_->message_trace().propagation_context();
    if (propagation) {
        context_.emplace(std::move(*propagation));
    }
    (void) root_->set_data_separator(' ');
    add_root_data("method", exchange.method_view());
    add_root_data("host", exchange.header("Host"));
    add_root_data("path", exchange.uri().path);
    add_root_data("content_type", exchange.header("Content-Type"));
    add_root_data("realIp", exchange.header("X-Real-Ip"));
    const std::string_view user_agent = exchange.header("User-Agent");
    if (!user_agent.empty()) {
        if (user_agent.size() > kMaxUserAgentBytes) {
            add_root_data("userAgentTruncated", "true");
        }
        add_root_data("userAgent", user_agent.substr(0, kMaxUserAgentBytes));
    }
    add_root_data("trace_context", invalid_fallback ? "invalid_fallback" : (inherited ? "continued" : "new"));
}

AccessRequestTelemetry::~AccessRequestTelemetry() {
    const auto finished = event::EventLoop::current().now();
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(finished - started_);
    const http::HttpResponseStats &response = exchange_->response_stats();
    if (metrics_) {
        metrics_->request_finished(response, duration);
    }

    std::array<char, std::numeric_limits<int>::digits10 + 3> status_buffer{};
    const auto status =
            std::to_chars(status_buffer.data(), status_buffer.data() + status_buffer.size(), response.status_code);
    const std::string_view status_text =
            status.ec == std::errc{} ? std::string_view(status_buffer.data(),
                                                        static_cast<std::size_t>(status.ptr - status_buffer.data()))
                                     : std::string_view("0");
    add_root_data("status", status_text);
    add_root_data("result", response_result(response));
    if (response.terminal_error != common::IoErr::None) {
        add_root_data("io_error", common::io_err_name(response.terminal_error));
    }
    if (root_ && root_->valid()) {
        if (!error_.empty()) {
            (void) root_->complete(error_);
        } else {
            const bool success = response.completed && response.terminal_error == common::IoErr::None &&
                                 response.status_code >= 200 && response.status_code < 400;
            (void) root_->complete(success ? cat::status::Success : cat::status::Fail);
        }
    }

    LOG(LOG_ACCESS, INFO) << "request completed"
                          << " trace_id=" << log::quoted(trace_id())
                          << " method=" << log::quoted(exchange_->method_view())
                          << " host=" << log::quoted(exchange_->header("Host"))
                          << " path=" << log::quoted(exchange_->uri().unparsed_uri)
                          << " project=" << log::quoted(project_) << " route=" << log::quoted(route_)
                          << " cluster=" << log::quoted(cluster_) << " upstream=" << log::quoted(upstream_)
                          << " status=" << response.status_code << " result=" << response_result(response)
                          << " error=" << log::quoted(error_)
                          << " duration_us=" << std::max<std::int64_t>(duration.count(), 0)
                          << " response_bytes=" << response.body_bytes_sent
                          << " io_error=" << common::io_err_name(response.terminal_error);
}

std::string_view AccessRequestTelemetry::copy_to_request_pool(std::string_view value) noexcept {
    if (value.empty()) {
        return {};
    }
    char *copy = exchange_->pool().alloc<char>(value.size());
    if (!copy) {
        return {};
    }
    std::memcpy(copy, value.data(), value.size());
    return {copy, value.size()};
}

void AccessRequestTelemetry::set_project(std::string_view project, std::string_view effective_host,
                                         std::string_view context_cluster) noexcept {
    project_ = copy_to_request_pool(project);
    cluster_ = copy_to_request_pool(context_cluster);
    add_root_data("project", project);
    add_root_data("effectiveHost", effective_host);
    if (!context_cluster.empty()) {
        add_root_data("cluster", context_cluster);
    }
    update_transaction_name();
}

void AccessRequestTelemetry::set_route(const CompiledRoute &route) noexcept {
    route_ = copy_to_request_pool(route.path);
    add_root_data("route", route.path);
    update_transaction_name();
}

void AccessRequestTelemetry::set_error(const AccessError &error) noexcept {
    if (!error_.empty()) {
        return;
    }
    error_ = copy_to_request_pool(error.name);
    add_root_data("error", error.name);
    if (root_ && root_->valid()) {
        auto event = root_->start_event("FiberException", error.name);
        if (event) {
            (void) event->add_data(error.message);
            (void) event->complete(cat::status::Error);
        }
    }
}

void AccessRequestTelemetry::set_upstream(const ProxyUpstreamEndpoint &endpoint) noexcept {
    upstream_ = copy_to_request_pool(endpoint.host_header);
    add_root_data("upstream", endpoint.host_header);
}

std::string_view AccessRequestTelemetry::trace_id() const noexcept {
    if (!context_) {
        return {};
    }
    return context_->root_message_id().empty() ? context_->message_id() : context_->root_message_id();
}

bool AccessRequestTelemetry::inject_response_headers(http::HttpHeaders &headers) const noexcept {
    const std::string_view id = trace_id();
    return id.empty() ||
           headers.set_view(kTraceIdHeader, id, kTraceIdLowcaseHeader.data(), kTraceIdHeaderHash) != nullptr;
}

bool AccessRequestTelemetry::inject_upstream_headers(http::HttpHeaders &headers) noexcept {
    if (!cat_client_ || !context_ || !context_->valid()) {
        return true;
    }
    auto remote = cat_client_->create_remote_context(*context_, {});
    if (!remote) {
        return true;
    }
    remote_context_.emplace(std::move(*remote));
    const std::string_view message_id = remote_context_->message_id();
    const std::string_view root_id =
            remote_context_->root_message_id().empty() ? message_id : remote_context_->root_message_id();
    const std::string_view parent_id = remote_context_->parent_message_id();
    if (message_id.empty() || root_id.empty() || parent_id.empty()) {
        return true;
    }
    if (!headers.set_view(kTraceIdHeader, root_id, kTraceIdLowcaseHeader.data(), kTraceIdHeaderHash) ||
        !headers.set_view(kParentSpanIdHeader, parent_id, kParentSpanIdLowcaseHeader.data(), kParentSpanIdHeaderHash) ||
        !headers.set_view(kSpanIdHeader, message_id, kSpanIdLowcaseHeader.data(), kSpanIdHeaderHash)) {
        return false;
    }
    if (root_ && root_->valid()) {
        auto event = root_->start_event("RemoteCall", "");
        if (event) {
            (void) event->add_data(message_id);
            (void) event->complete(cat::status::Success);
        }
    }
    return true;
}

void AccessRequestTelemetry::add_root_data(std::string_view key, std::string_view value) noexcept {
    if (root_ && root_->valid() && !value.empty()) {
        (void) root_->add_data(key, value);
    }
}

void AccessRequestTelemetry::update_transaction_name() noexcept {
    if (!root_ || !root_->valid() || project_.empty()) {
        return;
    }
    if (route_.empty()) {
        (void) root_->set_name(project_);
        return;
    }
    if (project_.size() > std::numeric_limits<std::size_t>::max() - route_.size()) {
        return;
    }
    const std::size_t size = project_.size() + route_.size();
    char *name = exchange_->pool().alloc<char>(size);
    if (!name) {
        return;
    }
    std::memcpy(name, project_.data(), project_.size());
    std::memcpy(name + project_.size(), route_.data(), route_.size());
    (void) root_->set_name(std::string_view(name, size));
}

} // namespace fiber::access_server
