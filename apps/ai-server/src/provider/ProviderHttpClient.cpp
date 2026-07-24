#include "ProviderHttpClient.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

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

ProviderHttpError error(ProviderHttpErrorCode code, common::IoErr io_error, const char *message) noexcept {
    return ProviderHttpError{.code = code, .io_error = io_error, .message = message};
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

void build_request_headers(const ProviderConnectionLease &connection, const ResolvedProviderAttempt &attempt,
                           bool stream, http::HttpHeaders &headers) {
    headers.set("Host", connection.host_header);
    headers.set_view("Content-Type", "application/json");
    headers.set_view("Accept", stream ? "text/event-stream" : "application/json");
    if (attempt.api_token) {
        std::string authorization;
        authorization.reserve(attempt.api_token->token.size() + 7);
        authorization.append("Bearer ");
        authorization.append(attempt.api_token->token);
        headers.set("Authorization", authorization);
    }
}

std::string header_copy(const http::HttpHeaders &headers, std::string_view name) {
    const std::string_view value = headers.get(name);
    return std::string(value);
}

} // namespace

async::Task<std::expected<BufferedProviderResponse, ProviderHttpError>>
ProviderHttpClient::execute_buffered(const ResolvedProviderAttempt &attempt, std::string_view route_key, bool stream,
                                     mem::IoBufChain request_body, mem::BufPool &request_pool,
                                     std::size_t max_response_bytes) noexcept {
    auto started = co_await start(attempt, route_key, stream, std::move(request_body), request_pool);
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
            co_return std::unexpected(
                    error(ProviderHttpErrorCode::ReadBody, chunk.error(), "failed to read provider response body"));
        }
        const bool complete = chunk->complete();
        if (chunk->readable_bytes() > max_response_bytes ||
            (response.body && response.body.readable() > max_response_bytes - chunk->readable_bytes())) {
            (void) upstream.abort(common::IoErr::MessageTooLarge);
            co_return std::unexpected(error(ProviderHttpErrorCode::ResponseTooLarge, common::IoErr::MessageTooLarge,
                                            "provider response body is too large"));
        }
        if (!append_chain(response.body, *chunk, max_response_bytes)) {
            (void) upstream.abort(common::IoErr::NoMem);
            co_return std::unexpected(error(ProviderHttpErrorCode::ReadBody, common::IoErr::NoMem,
                                            "failed to buffer provider response body"));
        }
        if (complete) {
            break;
        }
    }
    co_return std::move(response);
}

async::Task<std::expected<ProviderHttpResponseStream, ProviderHttpError>>
ProviderHttpClient::start(const ResolvedProviderAttempt &attempt, std::string_view route_key, bool stream,
                          mem::IoBufChain request_body, mem::BufPool &request_pool) noexcept {
    auto acquired = co_await connections_->acquire(attempt, route_key, kConnectTimeout);
    if (!acquired) {
        co_return std::unexpected(
                error(ProviderHttpErrorCode::Connect, acquired.error().io_error, acquired.error().message));
    }
    ProviderConnectionLease connection = std::move(*acquired);

    auto upstream = std::unique_ptr<http::ClientHttp1Exchange>(
            new (std::nothrow) http::ClientHttp1Exchange(*connection.connection, request_pool));
    if (!upstream) {
        co_return std::unexpected(
                error(ProviderHttpErrorCode::Connect, common::IoErr::NoMem, "failed to allocate provider exchange"));
    }
    http::HttpHeaders request_headers(request_pool);
    build_request_headers(connection, attempt, stream, request_headers);
    const std::size_t request_size = request_body.readable_bytes();
    http::Http1RequestHead request_head{
            .method = http::HttpMethod::Post,
            .target = connection.target,
            .headers = &request_headers,
            .body = http::HttpBodySpec::ContentLength(request_size),
    };
    auto sent_header = co_await upstream->send_header(request_head, request_size == 0, kProviderTimeout);
    if (!sent_header) {
        co_return std::unexpected(
                error(ProviderHttpErrorCode::SendHeader, sent_header.error(), "failed to send provider headers"));
    }
    if (request_size > 0) {
        auto sent_body = co_await upstream->write_body(std::move(request_body), kProviderTimeout);
        if (!sent_body || *sent_body != request_size) {
            co_return std::unexpected(error(ProviderHttpErrorCode::SendBody,
                                            sent_body ? common::IoErr::Invalid : sent_body.error(),
                                            "failed to send provider request body"));
        }
    }

    const http::Http1ResponseHead *response_head = nullptr;
    for (;;) {
        auto received = co_await upstream->read_header(kProviderTimeout);
        if (!received) {
            co_return std::unexpected(error(ProviderHttpErrorCode::ReadHeader, received.error(),
                                            "failed to read provider response headers"));
        }
        if (!(*received)->is_informational()) {
            response_head = *received;
            break;
        }
    }
    if (!response_head || response_head->status_code < 100 || response_head->status_code > 999) {
        (void) upstream->abort(common::IoErr::Invalid);
        co_return std::unexpected(error(ProviderHttpErrorCode::InvalidResponse, common::IoErr::Invalid,
                                        "invalid provider response status"));
    }

    co_return ProviderHttpResponseStream(std::move(connection), std::move(upstream), response_head->status_code,
                                         header_copy(response_head->headers, "content-type"),
                                         header_copy(response_head->headers, "retry-after"),
                                         header_copy(response_head->headers, "x-request-id"));
}

async::Task<common::IoResult<mem::IoBufChain>>
ProviderHttpResponseStream::read_body(std::size_t max_bytes, std::chrono::milliseconds timeout) noexcept {
    if (!upstream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await upstream_->read_body(max_bytes, timeout);
}

common::IoResult<void> ProviderHttpResponseStream::abort(common::IoErr reason) noexcept {
    if (!upstream_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return upstream_->abort(reason);
}

} // namespace fiber::ai_server
