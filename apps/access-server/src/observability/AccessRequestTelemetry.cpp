#include "AccessRequestTelemetry.h"
#include "AccessServerLogCategories.h"

#include "../execution/AccessResult.h"
#include "../routing/ProjectRouteSnapshot.h"
#include "../routing/ProxyAddressSelector.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <string_view>

#include <fiber/cat/CatClient.h>
#include <fiber/cat/Status.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/log/Log.h>

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

template<typename T>
void add_integer(cat::Transaction &transaction, std::string_view key, T value) noexcept {
    std::array<char, std::numeric_limits<T>::digits10 + 3> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (converted.ec == std::errc{}) {
        (void) transaction.add_data(
                key, std::string_view(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())));
    }
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

AccessProviderTransaction::AccessProviderTransaction(AccessProviderTransaction &&other) noexcept :
    transaction_(std::move(other.transaction_)) {}

AccessProviderTransaction &AccessProviderTransaction::operator=(AccessProviderTransaction &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    cancel_pending();
    transaction_ = std::move(other.transaction_);
    return *this;
}

AccessProviderTransaction::~AccessProviderTransaction() { cancel_pending(); }

bool AccessProviderTransaction::valid() const noexcept { return transaction_.valid(); }

void AccessProviderTransaction::add_upstream(std::string_view upstream, std::size_t attempt) noexcept {
    if (!valid()) {
        return;
    }
    (void) transaction_.add_data("upstream", upstream);
    add_integer(transaction_, "attempt", attempt);
}

void AccessProviderTransaction::add_connection_reuse(std::uint64_t reuse_count) noexcept {
    if (!valid()) {
        return;
    }
    add_integer(transaction_, "reuse_count", reuse_count);
}

void AccessProviderTransaction::fail(std::string_view phase, common::IoErr error) noexcept {
    if (!valid()) {
        return;
    }
    if (!phase.empty()) {
        (void) transaction_.add_data("phase", phase);
    }
    if (error != common::IoErr::None) {
        (void) transaction_.add_data("io_error", common::io_err_name(error));
    }
    (void) transaction_.complete(cat::status::Fail);
}

void AccessProviderTransaction::call_error(const Exception &exception, std::string_view phase,
                                           common::IoErr error) noexcept {
    if (!valid()) {
        return;
    }
    auto event = transaction_.start_event("CALL_ERROR", exception.name);
    if (event) {
        (void) event->add_data(exception.message);
        if (!phase.empty()) {
            (void) event->add_data("phase", phase);
        }
        if (error != common::IoErr::None) {
            (void) event->add_data("io_error", common::io_err_name(error));
        }
        (void) event->complete(cat::status::Error);
    }
    fail(phase, error);
}

void AccessProviderTransaction::complete(int status_code) noexcept {
    if (!valid()) {
        return;
    }
    add_integer(transaction_, "status", status_code);
    (void) transaction_.complete(status_code < 500 ? cat::status::Success : cat::status::Fail);
}

void AccessProviderTransaction::cancel_pending() noexcept {
    if (valid()) {
        fail("canceled", common::IoErr::Canceled);
    }
}

AccessRequestTelemetry::AccessRequestTelemetry(http::HttpExchange &exchange, AccessServerMetrics::Worker *metrics,
                                               cat::CatClient *cat_client) noexcept :
    script_heap_(exchange.pool()), script_context_(exchange, script_heap_), response_headers_(exchange.pool()),
    metrics_(metrics), started_(event::EventLoop::current().now()) {
    if (metrics_) {
        metrics_->request_started();
    }
    if (!cat_client) {
        return;
    }

    const cat::MessageTraceContext inbound = read_trace_context(exchange.request_headers());
    const bool inherited = has_inbound_context(inbound);
    bool invalid_fallback = false;
    auto created =
            cat_client->create_isolated_transaction(exchange.pool(), "URL", exchange.uri().path, {.context = inbound});
    if (!created && inherited &&
        (created.error() == cat::RecordError::InvalidContext || created.error() == cat::RecordError::LimitExceeded)) {
        invalid_fallback = true;
        created = cat_client->create_isolated_transaction(exchange.pool(), "URL", exchange.uri().path);
    }
    if (!created) {
        return;
    }
    root_ = std::move(*created);

    auto propagation = root_.message_trace().propagation_context();
    if (propagation) {
        context_.emplace(std::move(*propagation));
    }
    (void) root_.set_data_separator(' ');
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
    http::HttpExchange &exchange = script_context_.exchange();
    const http::HttpResponseStats &response = exchange.response_stats();
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
    if (response.terminal_error != common::IoErr::None) {
        add_root_data("io_error", common::io_err_name(response.terminal_error));
    }
    if (root_.valid()) {
        const bool success = !execution_failed_ && response.completed && response.terminal_error == common::IoErr::None;
        (void) root_.complete(success ? cat::status::Success : cat::status::Error);
    }

    LOG(LOG_ACCESS, INFO) << "request completed"
                          << " trace_id=" << log::quoted(trace_id())
                          << " method=" << log::quoted(exchange.method_view())
                          << " host=" << log::quoted(exchange.header("Host"))
                          << " path=" << log::quoted(exchange.uri().unparsed_uri)
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
    char *copy = script_context_.exchange().pool().alloc<char>(value.size());
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

void AccessRequestTelemetry::mark_failed(std::string_view error) noexcept {
    execution_failed_ = true;
    if (failure_recorded_) {
        return;
    }
    failure_recorded_ = true;
    error_ = copy_to_request_pool(error);
    add_root_data("error", error);
}

void AccessRequestTelemetry::record_exception(const Exception &exception) noexcept {
    mark_failed(exception.name);
    if (exception_recorded_) {
        return;
    }
    exception_recorded_ = true;
    if (root_.valid()) {
        auto event = root_.start_event("FiberException", exception.name);
        if (event) {
            (void) event->add_data(exception.message);
            (void) event->complete(cat::status::Error);
        }
    }
}

void AccessRequestTelemetry::record_upstream_exception(const Exception &exception) noexcept {
    mark_failed(exception.name);
}

void AccessRequestTelemetry::record_response_error(common::IoErr error) noexcept {
    mark_failed("RESPONSE_ERROR");
    if (response_error_recorded_) {
        return;
    }
    response_error_recorded_ = true;
    if (!root_.valid()) {
        return;
    }
    const std::string_view error_name = common::io_err_name(error);
    auto event = root_.start_event("ResponseError", error_name);
    if (event) {
        (void) event->add_data("io_error", error_name);
        (void) event->complete(cat::status::Error);
    }
}

void AccessRequestTelemetry::mark_io_error(common::IoErr error) noexcept { mark_failed(common::io_err_name(error)); }

void AccessRequestTelemetry::set_upstream(const ProxyUpstreamEndpoint &endpoint) noexcept {
    upstream_ = copy_to_request_pool(endpoint.host_header);
    add_root_data("upstream", endpoint.host_header);
}

AccessProviderTransaction AccessRequestTelemetry::start_provider_transaction(std::string_view name) noexcept {
    if (!root_.valid()) {
        return {};
    }
    auto transaction = root_.start_transaction("Access.Provider", name);
    if (!transaction) {
        return {};
    }
    return AccessProviderTransaction(std::move(*transaction));
}

std::string_view AccessRequestTelemetry::trace_id() const noexcept {
    if (!context_) {
        return {};
    }
    return context_->root_message_id.empty() ? context_->message_id : context_->root_message_id;
}

bool AccessRequestTelemetry::finalize_response_headers() noexcept {
    const std::string_view id = trace_id();
    return id.empty() ||
           response_headers_.set_view(kTraceIdHeader, id, kTraceIdLowcaseHeader.data(), kTraceIdHeaderHash) != nullptr;
}

bool AccessRequestTelemetry::inject_upstream_headers(http::HttpHeaders &headers,
                                                     AccessProviderTransaction &provider) noexcept {
    if (!root_.valid() || !context_ || context_->message_id.empty()) {
        return true;
    }
    cat::Transaction &parent = provider.valid() ? provider.transaction_ : root_;
    auto remote = parent.message_trace().create_remote_context(script_context_.exchange().pool());
    if (!remote) {
        return true;
    }
    const std::string_view message_id = remote->message_id;
    const std::string_view root_id = remote->root_message_id.empty() ? message_id : remote->root_message_id;
    const std::string_view parent_id = remote->parent_message_id;
    if (message_id.empty() || root_id.empty() || parent_id.empty()) {
        return true;
    }
    if (!headers.set_view(kTraceIdHeader, root_id, kTraceIdLowcaseHeader.data(), kTraceIdHeaderHash) ||
        !headers.set_view(kParentSpanIdHeader, parent_id, kParentSpanIdLowcaseHeader.data(), kParentSpanIdHeaderHash) ||
        !headers.set_view(kSpanIdHeader, message_id, kSpanIdLowcaseHeader.data(), kSpanIdHeaderHash)) {
        return false;
    }
    auto event = parent.start_event("RemoteCall", "");
    if (event) {
        (void) event->add_data(message_id);
        (void) event->complete(cat::status::Success);
    }
    return true;
}

void AccessRequestTelemetry::add_root_data(std::string_view key, std::string_view value) noexcept {
    if (root_.valid() && !value.empty()) {
        (void) root_.add_data(key, value);
    }
}

void AccessRequestTelemetry::update_transaction_name() noexcept {
    if (!root_.valid() || project_.empty()) {
        return;
    }
    if (route_.empty()) {
        (void) root_.set_name(project_);
        return;
    }
    if (project_.size() > std::numeric_limits<std::size_t>::max() - route_.size()) {
        return;
    }
    const std::size_t size = project_.size() + route_.size();
    char *name = script_context_.exchange().pool().alloc<char>(size);
    if (!name) {
        return;
    }
    std::memcpy(name, project_.data(), project_.size());
    std::memcpy(name + project_.size(), route_.data(), route_.size());
    (void) root_.set_name(std::string_view(name, size));
}

} // namespace fiber::access_server
