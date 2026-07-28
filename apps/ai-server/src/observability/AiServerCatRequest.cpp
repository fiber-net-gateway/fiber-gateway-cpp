#include "AiServerCatRequest.h"

#include <array>
#include <charconv>
#include <limits>
#include <utility>

#include <fiber/cat/CatClient.h>
#include <fiber/cat/Status.h>
#include <http/HttpExchange.h>
#include <http/HttpHeaderHash.h>
#include <http/HttpHeaders.h>

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

bool has_inbound_context(const cat::MessageTraceContext &context) noexcept {
    return !context.message_id.empty() || !context.root_message_id.empty() || !context.parent_message_id.empty();
}

bool can_fallback_from(cat::RecordError error) noexcept {
    return error == cat::RecordError::InvalidContext || error == cat::RecordError::LimitExceeded;
}

void add_status_code(cat::Transaction &transaction, int status_code) noexcept {
    std::array<char, std::numeric_limits<int>::digits10 + 3> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), status_code);
    if (converted.ec == std::errc{}) {
        (void) transaction.add_data(
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

bool inject_cat_headers(http::HttpHeaders &headers, const cat::PropagationContext *context,
                        std::string_view trace_state) noexcept {
    if (!context || !context->valid()) {
        return true;
    }
    const std::string_view message_id = context->message_id();
    const std::string_view root_id = context->root_message_id().empty() ? message_id : context->root_message_id();
    const std::string_view parent_id = context->parent_message_id();
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
    exchange_(&exchange), client_(client) {
    if (!client_) {
        return;
    }

    const std::string_view inbound_trace_state =
            exchange.request_headers().get(kTraceStateHeader, kTraceStateHeaderHash);
    if (inbound_trace_state.size() <= kMaxAiServerTraceStateBytes) {
        trace_state_ = inbound_trace_state;
    }

    const cat::MessageTraceContext inbound = read_cat_trace_context(exchange.request_headers());
    const bool inherited = has_inbound_context(inbound);
    bool invalid_fallback = false;
    auto created = cat::MessageTrace::create(*client_, {}, inbound);
    if (!created && inherited && can_fallback_from(created.error())) {
        invalid_fallback = true;
        created = cat::MessageTrace::create(*client_);
    }
    if (!created) {
        return;
    }
    trace_.emplace(std::move(*created));

    auto propagation = trace_->propagation_context();
    if (propagation) {
        context_.emplace(std::move(*propagation));
    }

    auto transaction = trace_->create_transaction("URL", exchange.uri().path);
    if (!transaction) {
        context_.reset();
        trace_.reset();
        return;
    }
    root_.emplace(std::move(*transaction));
    (void) root_->add_data("method", exchange.method_view());
    const std::string_view host = exchange.request_headers().get(kHostHeader, kHostHeaderHash);
    if (!host.empty()) {
        (void) root_->add_data("host", host);
    }
    const std::string_view content_type = exchange.request_headers().get(kContentTypeHeader, kContentTypeHeaderHash);
    if (!content_type.empty()) {
        (void) root_->add_data("content_type", content_type);
    }
    (void) root_->add_data("trace_context",
                           invalid_fallback ? std::string_view("invalid_fallback")
                                            : (inherited ? std::string_view("continued") : std::string_view("new")));
}

AiServerCatRequest::~AiServerCatRequest() {
    if (!root_ || !root_->valid()) {
        return;
    }
    const http::HttpResponseStats &response = exchange_->response_stats();
    add_status_code(*root_, response.status_code);
    const bool success = response.completed && response.terminal_error == common::IoErr::None &&
                         response.status_code >= 200 && response.status_code < 400;
    (void) root_->complete(success ? cat::status::Success : cat::status::Fail);
}

std::string_view AiServerCatRequest::request_id() const noexcept {
    if (!context_) {
        return {};
    }
    return context_->root_message_id().empty() ? context_->message_id() : context_->root_message_id();
}

void AiServerCatRequest::inject_response_header(http::HttpHeaders &headers) const noexcept {
    const std::string_view trace_id = request_id();
    if (!trace_id.empty()) {
        (void) headers.set_view(kTraceIdHeader, trace_id, kTraceIdLowcaseHeader.data(), kTraceIdHeaderHash);
    }
}

std::expected<cat::PropagationContext, cat::RecordError>
AiServerCatRequest::create_remote_context(cat::Transaction *parent) noexcept {
    if (!client_ || !context_ || !context_->valid()) {
        return std::unexpected(cat::RecordError::InvalidContext);
    }
    auto remote = client_->create_remote_context(*context_, {});
    if (!remote) {
        return std::unexpected(remote.error());
    }

    cat::Transaction *event_parent = parent && parent->valid() ? parent : root_transaction();
    if (event_parent) {
        auto event = event_parent->start_event("RemoteCall", "");
        if (event) {
            (void) event->add_data(remote->message_id());
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
