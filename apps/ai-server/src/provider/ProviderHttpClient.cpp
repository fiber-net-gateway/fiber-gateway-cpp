#include "ProviderHttpClient.h"

#include "../observability/AiServerCatRequest.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include <event/EventLoop.h>
#include <fiber/cat/Status.h>
#include <fiber/cat/Transaction.h>
#include <http/ClientHttp1Exchange.h>
#include <http/ClientHttp1Types.h>
#include <http/HttpBodySpec.h>
#include <http/HttpHeaderHash.h>
#include <http/HttpHeaders.h>

namespace fiber::ai_server {
namespace {

constexpr std::chrono::seconds kProviderTimeout{300};
constexpr std::chrono::seconds kConnectTimeout{10};
constexpr std::size_t kResponseChunkSize = 64 * 1024;
constexpr std::string_view kBearerPrefix = "Bearer ";
constexpr std::string_view kAuthorizationName = "Authorization";
constexpr std::string_view kAuthorizationLowcaseName = "authorization";
constexpr std::uint64_t kAuthorizationHash = http::http_header_name_hash(kAuthorizationLowcaseName);

ProviderHttpError error(ProviderHttpErrorCode code, common::IoErr io_error, const char *message,
                        std::uint64_t failed_service_peer_id = 0, ProviderHttpTiming timing = {},
                        bool dns_backoff_hit = false) noexcept {
    return ProviderHttpError{
            .code = code,
            .io_error = io_error,
            .message = message,
            .failed_service_peer_id = failed_service_peer_id,
            .dns_backoff_hit = dns_backoff_hit,
            .timing = timing,
    };
}

void add_reuse_count(cat::Transaction &transaction, std::uint64_t reuse_count) noexcept {
    if (!transaction.valid()) {
        return;
    }
    std::array<char, std::numeric_limits<std::uint64_t>::digits10 + 2> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), reuse_count);
    if (converted.ec == std::errc{}) {
        (void) transaction.add_data(
                "reuse_count",
                std::string_view(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())));
    }
}

void add_remote_call(cat::Transaction &transaction, const cat::MessageTraceContext &context) noexcept {
    if (!transaction.valid()) {
        return;
    }
    auto event = transaction.start_event("RemoteCall", "");
    if (!event) {
        return;
    }
    (void) event->add_data(context.message_id);
    (void) event->complete(cat::status::Success);
}

ProviderHttpErrorCode http_error_code(ProviderConnectionErrorCode code) noexcept {
    switch (code) {
        case ProviderConnectionErrorCode::InvalidEndpoint:
            return ProviderHttpErrorCode::InvalidEndpoint;
        case ProviderConnectionErrorCode::NoServiceEndpoint:
            return ProviderHttpErrorCode::NoServiceEndpoint;
        case ProviderConnectionErrorCode::Dns:
            return ProviderHttpErrorCode::Dns;
        case ProviderConnectionErrorCode::PoolShutdown:
            return ProviderHttpErrorCode::PoolShutdown;
        case ProviderConnectionErrorCode::Connect:
            return ProviderHttpErrorCode::Connect;
    }
    return ProviderHttpErrorCode::Connect;
}

std::chrono::microseconds elapsed(std::chrono::steady_clock::time_point start,
                                  std::chrono::steady_clock::time_point end) noexcept {
    return std::max(std::chrono::duration_cast<std::chrono::microseconds>(end - start),
                    std::chrono::microseconds::zero());
}

bool ensure_capacity(mem::IoBuf &buffer, std::size_t required, std::size_t max_capacity) noexcept {
    if (buffer && buffer.capacity() >= required) {
        return true;
    }
    std::size_t capacity = buffer ? buffer.capacity() : 0;
    capacity = std::max<std::size_t>(capacity, 4096);
    while (capacity < required) {
        const std::size_t next = capacity > max_capacity / 2 ? max_capacity : capacity * 2;
        if (next <= capacity) {
            return false;
        }
        capacity = next;
    }
    mem::IoBuf replacement = mem::IoBuf::allocate(capacity);
    if (!replacement) {
        return false;
    }
    if (buffer && buffer.readable() > 0) {
        std::memcpy(replacement.writable_data(), buffer.readable_data(), buffer.readable());
        replacement.commit(buffer.readable());
    }
    buffer = std::move(replacement);
    return true;
}

bool append_chain(mem::IoBuf &buffer, mem::IoBufChain &chunk, std::size_t max_bytes) noexcept {
    const std::size_t bytes = chunk.readable_bytes();
    if (bytes > max_bytes || (buffer && buffer.readable() > max_bytes - bytes)) {
        return false;
    }
    const std::size_t required = (buffer ? buffer.readable() : 0) + bytes;
    if (!ensure_capacity(buffer, std::max<std::size_t>(required, 1), max_bytes)) {
        return false;
    }
    while (const mem::IoBuf *part = chunk.first_readable()) {
        const std::size_t size = part->readable();
        std::memcpy(buffer.writable_data(), part->readable_data(), size);
        buffer.commit(size);
        chunk.consume_and_compact(size);
    }
    return true;
}

common::IoResult<void> build_request_headers(const ProviderConnectionLease &connection,
                                             const ResolvedProviderAttempt &attempt, bool stream,
                                             http::HttpHeaders &headers, const cat::MessageTraceContext *cat_context,
                                             std::string_view trace_state) noexcept {
    if (!headers.set("Host", connection.host_header) || !headers.set_view("Content-Type", "application/json") ||
        !headers.set_view("Accept", stream ? "text/event-stream" : "application/json")) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (attempt.api_token) {
        const std::string &token = attempt.api_token->token;
        if (token.size() > std::numeric_limits<std::size_t>::max() - kBearerPrefix.size()) {
            return std::unexpected(common::IoErr::MessageTooLarge);
        }
        const std::size_t authorization_size = kBearerPrefix.size() + token.size();
        auto *authorization = headers.pool().alloc<char>(authorization_size);
        if (!authorization) {
            return std::unexpected(common::IoErr::NoMem);
        }
        std::memcpy(authorization, kBearerPrefix.data(), kBearerPrefix.size());
        std::memcpy(authorization + kBearerPrefix.size(), token.data(), token.size());

        if (!headers.set_view(kAuthorizationName, std::string_view(authorization, authorization_size),
                              kAuthorizationLowcaseName.data(), kAuthorizationHash)) {
            return std::unexpected(common::IoErr::NoMem);
        }
    }
    (void) inject_cat_headers(headers, cat_context, trace_state);
    return {};
}

std::string header_copy(const http::HttpHeaders &headers, std::string_view name) {
    const std::string_view value = headers.get(name);
    return std::string(value);
}

} // namespace

std::string_view provider_http_error_code_name(ProviderHttpErrorCode code) noexcept {
    switch (code) {
        case ProviderHttpErrorCode::InvalidEndpoint:
            return "invalid_endpoint";
        case ProviderHttpErrorCode::NoServiceEndpoint:
            return "no_service_endpoint";
        case ProviderHttpErrorCode::Dns:
            return "dns";
        case ProviderHttpErrorCode::PoolShutdown:
            return "pool_shutdown";
        case ProviderHttpErrorCode::Connect:
            return "connect";
        case ProviderHttpErrorCode::SendHeader:
            return "send_header";
        case ProviderHttpErrorCode::SendBody:
            return "send_body";
        case ProviderHttpErrorCode::ReadHeader:
            return "read_header";
        case ProviderHttpErrorCode::ReadBody:
            return "read_body";
        case ProviderHttpErrorCode::ResponseTooLarge:
            return "response_too_large";
        case ProviderHttpErrorCode::InvalidResponse:
            return "invalid_response";
        case ProviderHttpErrorCode::Count:
            break;
    }
    return "unknown";
}

async::Task<std::expected<BufferedProviderResponse, ProviderHttpError>>
ProviderHttpClient::execute_buffered(const ResolvedProviderAttempt &attempt, bool stream, mem::IoBufChain request_body,
                                     mem::BufPool &request_pool, std::size_t max_response_bytes,
                                     ProviderServiceSelection service_selection, cat::Transaction &cat_transaction,
                                     std::string_view trace_state) noexcept {
    auto started = co_await start(attempt, stream, std::move(request_body), request_pool, service_selection,
                                  cat_transaction, trace_state);
    if (!started) {
        co_return std::unexpected(started.error());
    }
    ProviderHttpResponseStream upstream = std::move(*started);
    BufferedProviderResponse response{
            .status_code = upstream.status_code(),
            .content_type = std::string(upstream.content_type()),
            .retry_after = std::string(upstream.retry_after()),
            .request_id = std::string(upstream.request_id()),
    };
    for (;;) {
        auto chunk = co_await upstream.read_body(kResponseChunkSize, kProviderTimeout);
        if (!chunk) {
            const std::uint64_t failed_service_peer_id = upstream.service_peer_id();
            const ProviderHttpTiming timing = upstream.timing();
            upstream.report_instance(InstanceReportOutcome::Failure);
            co_return std::unexpected(error(ProviderHttpErrorCode::ReadBody, chunk.error(),
                                            "failed to read provider response body", failed_service_peer_id, timing));
        }
        const bool complete = chunk->complete();
        if (chunk->readable_bytes() > max_response_bytes ||
            (response.body && response.body.readable() > max_response_bytes - chunk->readable_bytes())) {
            const ProviderHttpTiming timing = upstream.timing();
            (void) upstream.abort(common::IoErr::MessageTooLarge);
            upstream.report_instance(InstanceReportOutcome::Neutral);
            co_return std::unexpected(error(ProviderHttpErrorCode::ResponseTooLarge, common::IoErr::MessageTooLarge,
                                            "provider response body is too large", 0, timing));
        }
        if (!append_chain(response.body, *chunk, max_response_bytes)) {
            const ProviderHttpTiming timing = upstream.timing();
            (void) upstream.abort(common::IoErr::NoMem);
            upstream.report_instance(InstanceReportOutcome::Neutral);
            co_return std::unexpected(error(ProviderHttpErrorCode::ReadBody, common::IoErr::NoMem,
                                            "failed to buffer provider response body", 0, timing));
        }
        if (complete) {
            break;
        }
    }
    response.timing = upstream.timing();
    response.load_balance = upstream.take_load_balance();
    co_return std::move(response);
}

async::Task<std::expected<ProviderHttpResponseStream, ProviderHttpError>>
ProviderHttpClient::start(const ResolvedProviderAttempt &attempt, bool stream, mem::IoBufChain request_body,
                          mem::BufPool &request_pool, ProviderServiceSelection service_selection,
                          cat::Transaction &cat_transaction, std::string_view trace_state) noexcept {
    auto acquired = co_await connections_->acquire(attempt, kConnectTimeout, service_selection);
    if (!acquired) {
        co_return std::unexpected(error(http_error_code(acquired.error().code), acquired.error().io_error,
                                        acquired.error().message, acquired.error().failed_service_peer_id, {},
                                        acquired.error().dns_backoff_hit));
    }
    ProviderConnectionLease connection = std::move(*acquired);
    add_reuse_count(cat_transaction, connection.connection->request_count());
    auto remote_context = cat_transaction.message_trace().create_remote_context(request_pool);
    if (remote_context) {
        add_remote_call(cat_transaction, *remote_context);
    }

    auto upstream = std::unique_ptr<http::ClientHttp1Exchange>(
            new (std::nothrow) http::ClientHttp1Exchange(*connection.connection, request_pool));
    if (!upstream) {
        co_return std::unexpected(
                error(ProviderHttpErrorCode::Connect, common::IoErr::NoMem, "failed to allocate provider exchange"));
    }
    http::HttpHeaders request_headers(request_pool);
    auto built_headers = build_request_headers(connection, attempt, stream, request_headers,
                                               remote_context ? &*remote_context : nullptr, trace_state);
    if (!built_headers) {
        co_return std::unexpected(error(ProviderHttpErrorCode::SendHeader, built_headers.error(),
                                        "failed to build provider request headers"));
    }
    const std::size_t request_size = request_body.readable_bytes();
    http::Http1RequestHead request_head{
            .method = http::HttpMethod::Post,
            .target = connection.target,
            .headers = &request_headers,
            .body = http::HttpBodySpec::ContentLength(request_size),
    };
    const auto request_send_started = event::EventLoop::current().now();
    auto sent_header = co_await upstream->send_header(request_head, request_size == 0, kProviderTimeout);
    if (!sent_header) {
        const std::uint64_t failed_service_peer_id = connection.load_balance.peer_id();
        connection.load_balance.report(InstanceReportOutcome::Failure);
        co_return std::unexpected(error(ProviderHttpErrorCode::SendHeader, sent_header.error(),
                                        "failed to send provider headers", failed_service_peer_id));
    }
    if (request_size > 0) {
        auto sent_body = co_await upstream->write_all(std::move(request_body), kProviderTimeout);
        if (!sent_body || *sent_body != request_size) {
            const std::uint64_t failed_service_peer_id = connection.load_balance.peer_id();
            connection.load_balance.report(InstanceReportOutcome::Failure);
            co_return std::unexpected(error(ProviderHttpErrorCode::SendBody,
                                            sent_body ? common::IoErr::Invalid : sent_body.error(),
                                            "failed to send provider request body", failed_service_peer_id));
        }
    }

    const http::Http1ResponseHead *response_head = nullptr;
    for (;;) {
        auto received = co_await upstream->read_header(kProviderTimeout);
        if (!received) {
            const std::uint64_t failed_service_peer_id = connection.load_balance.peer_id();
            connection.load_balance.report(InstanceReportOutcome::Failure);
            co_return std::unexpected(error(ProviderHttpErrorCode::ReadHeader, received.error(),
                                            "failed to read provider response headers", failed_service_peer_id));
        }
        if (!(*received)->is_informational()) {
            response_head = *received;
            break;
        }
    }
    ProviderHttpTiming timing{
            .time_to_response_header = elapsed(request_send_started, event::EventLoop::current().now()),
            .response_header_observed = true,
    };
    if (!response_head || response_head->status_code < 100 || response_head->status_code > 999) {
        (void) upstream->abort(common::IoErr::Invalid);
        const std::uint64_t failed_service_peer_id = connection.load_balance.peer_id();
        connection.load_balance.report(InstanceReportOutcome::Failure);
        co_return std::unexpected(error(ProviderHttpErrorCode::InvalidResponse, common::IoErr::Invalid,
                                        "invalid provider response status", failed_service_peer_id, timing));
    }

    co_return ProviderHttpResponseStream(
            std::move(connection), std::move(upstream), response_head->status_code,
            header_copy(response_head->headers, "content-type"), header_copy(response_head->headers, "retry-after"),
            header_copy(response_head->headers, "x-request-id"), request_send_started, timing);
}

async::Task<common::IoResult<mem::IoBufChain>>
ProviderHttpResponseStream::read_body(std::size_t max_bytes, std::chrono::milliseconds timeout) noexcept {
    if (!upstream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    auto chunk = co_await upstream_->read_body(max_bytes, timeout);
    if (!chunk) {
        co_return std::unexpected(chunk.error());
    }
    const auto observed_at = event::EventLoop::current().now();
    if (!first_body_observed_ && chunk->readable_bytes() != 0) {
        first_body_observed_at_ = observed_at;
        first_body_observed_ = true;
    }
    if (!timing_.body_transfer_observed && first_body_observed_ && chunk->complete()) {
        timing_.body_transfer = elapsed(first_body_observed_at_, observed_at);
        timing_.body_transfer_observed = true;
    }
    co_return std::move(*chunk);
}

void ProviderHttpResponseStream::observe_first_token() noexcept {
    if (timing_.first_token_observed) {
        return;
    }
    timing_.time_to_first_token = elapsed(request_send_started_, event::EventLoop::current().now());
    timing_.first_token_observed = true;
}

common::IoResult<void> ProviderHttpResponseStream::abort(common::IoErr reason) noexcept {
    if (!upstream_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return upstream_->abort(reason);
}

} // namespace fiber::ai_server
