#include "AiServerCatRequest.h"

#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <utility>

#include <fiber/cat/CatClient.h>
#include <fiber/cat/Status.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/HttpHeaders.h>

namespace fiber::ai_server {

namespace {

constexpr std::string_view kTraceIdHeader = "HI-TRACE-ID";
constexpr std::string_view kTraceIdLowcaseHeader = "hi-trace-id";
constexpr std::uint64_t kTraceIdHeaderHash = http::http_header_name_hash(kTraceIdLowcaseHeader);
constexpr std::string_view kParentSpanIdHeader = "HI-SPAN-ID-PARENT";
constexpr std::string_view kParentSpanIdLowcaseHeader = "hi-span-id-parent";
constexpr std::uint64_t kParentSpanIdHeaderHash = http::http_header_name_hash(kParentSpanIdLowcaseHeader);
constexpr std::string_view kSpanIdHeader = "HI-SPAN-ID";
constexpr std::string_view kSpanIdLowcaseHeader = "hi-span-id";
constexpr std::uint64_t kSpanIdHeaderHash = http::http_header_name_hash(kSpanIdLowcaseHeader);
constexpr std::string_view kTraceStateHeader = "tracestate";
constexpr std::uint64_t kTraceStateHeaderHash = http::http_header_name_hash(kTraceStateHeader);
constexpr std::string_view kHostHeader = "host";
constexpr std::uint64_t kHostHeaderHash = http::http_header_name_hash(kHostHeader);
constexpr std::string_view kContentTypeHeader = "content-type";
constexpr std::uint64_t kContentTypeHeaderHash = http::http_header_name_hash(kContentTypeHeader);
constexpr std::string_view kUserAgentHeader = "user-agent";
constexpr std::uint64_t kUserAgentHeaderHash = http::http_header_name_hash(kUserAgentHeader);

bool has_inbound_context(const cat::MessageTraceContext &context) noexcept {
    return !context.message_id.empty() || !context.root_message_id.empty() || !context.parent_message_id.empty();
}

bool can_fallback_from(cat::RecordError error) noexcept {
    return error == cat::RecordError::InvalidContext || error == cat::RecordError::LimitExceeded;
}

void add_status_code(AiServerCatRequest &request, int status_code) noexcept {
    std::array<char, std::numeric_limits<int>::digits10 + 3> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), status_code);
    if (converted.ec == std::errc{}) {
        (void) request.add_root_data(
                "status", std::string_view(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())));
    }
}

void remove_cat_id_headers(http::HttpHeaders &headers) noexcept {
    headers.remove(kTraceIdLowcaseHeader, kTraceIdHeaderHash);
    headers.remove(kParentSpanIdLowcaseHeader, kParentSpanIdHeaderHash);
    headers.remove(kSpanIdLowcaseHeader, kSpanIdHeaderHash);
}

} // namespace

cat::MessageTraceContext read_cat_trace_context(const http::HttpHeaders &headers) noexcept {
    return {
            .message_id = headers.get(kSpanIdLowcaseHeader, kSpanIdHeaderHash),
            .root_message_id = headers.get(kTraceIdLowcaseHeader, kTraceIdHeaderHash),
            .parent_message_id = headers.get(kParentSpanIdLowcaseHeader, kParentSpanIdHeaderHash),
    };
}

bool inject_cat_headers(http::HttpHeaders &headers, const cat::MessageTraceContext *context,
                        std::string_view trace_state) noexcept {
    if (!context) {
        return true;
    }
    const std::string_view message_id = context->message_id;
    const std::string_view root_id = context->root_message_id.empty() ? message_id : context->root_message_id;
    const std::string_view parent_id = context->parent_message_id;
    if (message_id.empty() || root_id.empty() || parent_id.empty()) {
        return false;
    }

    if (!headers.set_view(kTraceIdHeader, root_id, kTraceIdLowcaseHeader.data(), kTraceIdHeaderHash) ||
        !headers.set_view(kParentSpanIdHeader, parent_id, kParentSpanIdLowcaseHeader.data(), kParentSpanIdHeaderHash) ||
        !headers.set_view(kSpanIdHeader, message_id, kSpanIdLowcaseHeader.data(), kSpanIdHeaderHash)) {
        remove_cat_id_headers(headers);
        return false;
    }
    if (!trace_state.empty() && trace_state.size() <= kMaxAiServerTraceStateBytes &&
        !headers.set_view(kTraceStateHeader, trace_state, kTraceStateHeader.data(), kTraceStateHeaderHash)) {
        headers.remove(kTraceStateHeader, kTraceStateHeaderHash);
        return false;
    }
    return true;
}

AiServerCatRequest::AiServerCatRequest(http::HttpExchange &exchange, cat::CatClient *client) noexcept :
    exchange_(&exchange) {
    if (!client) {
        return;
    }

    const http::HttpHeaders &request_headers = exchange.request_headers();
    const std::string_view inbound_trace_state = request_headers.get(kTraceStateHeader, kTraceStateHeaderHash);
    if (inbound_trace_state.size() <= kMaxAiServerTraceStateBytes) {
        trace_state_ = inbound_trace_state;
    }

    const cat::MessageTraceContext inbound = read_cat_trace_context(request_headers);
    const bool inherited = has_inbound_context(inbound);
    bool invalid_fallback = false;
    auto created =
            client->create_isolated_transaction(exchange.pool(), "URL", exchange.uri().path, {.context = inbound});
    if (!created && inherited && can_fallback_from(created.error())) {
        invalid_fallback = true;
        created = client->create_isolated_transaction(exchange.pool(), "URL", exchange.uri().path);
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
    (void) add_root_data("method", exchange.method_view());
    const std::string_view host = request_headers.get(kHostHeader, kHostHeaderHash);
    if (!host.empty()) {
        (void) add_root_data("host", host);
    }
    const std::string_view content_type = request_headers.get(kContentTypeHeader, kContentTypeHeaderHash);
    if (!content_type.empty()) {
        (void) add_root_data("content_type", content_type);
    }
    (void) add_root_data("trace_context",
                         invalid_fallback ? std::string_view("invalid_fallback")
                                          : (inherited ? std::string_view("continued") : std::string_view("new")));
    user_agent_ = request_headers.get(kUserAgentHeader, kUserAgentHeaderHash);
}

AiServerCatRequest::~AiServerCatRequest() {
    if (!root_ || !root_->valid()) {
        return;
    }
    const http::HttpResponseStats &response = exchange_->response_stats();
    add_status_code(*this, response.status_code);
    if (!user_agent_.empty()) {
        if (user_agent_.size() > kMaxAiServerCatUserAgentBytes) {
            (void) add_root_data("user_agent_truncated", "true");
        }
        (void) add_root_data("user_agent", user_agent_.substr(0, kMaxAiServerCatUserAgentBytes));
    }
    const bool success = response.completed && response.terminal_error == common::IoErr::None &&
                         response.status_code >= 200 && response.status_code < 400;
    (void) root_->complete(success ? cat::status::Success : cat::status::Fail);
}

std::string_view AiServerCatRequest::request_id() const noexcept {
    if (!context_) {
        return {};
    }
    return context_->root_message_id.empty() ? context_->message_id : context_->root_message_id;
}

void AiServerCatRequest::inject_response_header(http::HttpHeaders &headers) const noexcept {
    const std::string_view trace_id = request_id();
    if (!trace_id.empty()) {
        (void) headers.set_view(kTraceIdHeader, trace_id, kTraceIdLowcaseHeader.data(), kTraceIdHeaderHash);
    }
}

cat::RecordError AiServerCatRequest::add_root_data(std::string_view key, std::string_view value) noexcept {
    cat::Transaction *root = root_transaction();
    return root ? root->add_data(key, value) : cat::RecordError::Completed;
}

cat::RecordError AiServerCatRequest::set_root_model_name(std::string_view model) noexcept {
    cat::Transaction *root = root_transaction();
    if (!root) {
        return cat::RecordError::Completed;
    }
    if (model.empty()) {
        return cat::RecordError::InvalidArgument;
    }

    const std::string_view path = exchange_->uri().path;
    if (path.size() == std::numeric_limits<std::size_t>::max() ||
        model.size() > std::numeric_limits<std::size_t>::max() - path.size() - 1) {
        return cat::RecordError::LimitExceeded;
    }
    const std::size_t size = path.size() + 1 + model.size();
    char *name = exchange_->pool().alloc<char>(size);
    if (!name) {
        return cat::RecordError::NoMemory;
    }
    std::memcpy(name, path.data(), path.size());
    name[path.size()] = ':';
    std::memcpy(name + path.size() + 1, model.data(), model.size());
    return root->set_name(std::string_view(name, size));
}

std::expected<cat::MessageTraceContext, cat::RecordError>
AiServerCatRequest::create_remote_context(cat::Transaction *parent) noexcept {
    return create_remote_context(exchange_->pool(), parent);
}

std::expected<cat::MessageTraceContext, cat::RecordError>
AiServerCatRequest::create_remote_context(mem::BufPool &destination_pool, cat::Transaction *parent) noexcept {
    if (!root_ || !root_->valid() || !context_ || context_->message_id.empty()) {
        return std::unexpected(cat::RecordError::InvalidContext);
    }
    auto remote = root_->message_trace().create_remote_context(destination_pool);
    if (!remote) {
        return std::unexpected(remote.error());
    }

    cat::Transaction *event_parent = parent && parent->valid() ? parent : root_transaction();
    if (event_parent) {
        auto event = event_parent->start_event("RemoteCall", "");
        if (event) {
            (void) event->add_data(remote->message_id);
            (void) event->complete(cat::status::Success);
        }
    }
    return remote;
}

std::expected<cat::Event, cat::RecordError> AiServerCatRequest::start_event(std::string_view type,
                                                                            std::string_view name) noexcept {
    cat::Transaction *root = root_transaction();
    if (!root) {
        return std::unexpected(cat::RecordError::Completed);
    }
    return root->start_event(type, name);
}

std::expected<cat::Transaction, cat::RecordError>
AiServerCatRequest::start_transaction(std::string_view type, std::string_view name) noexcept {
    cat::Transaction *root = root_transaction();
    if (!root) {
        return std::unexpected(cat::RecordError::Completed);
    }
    return root->start_transaction(type, name);
}

} // namespace fiber::ai_server
