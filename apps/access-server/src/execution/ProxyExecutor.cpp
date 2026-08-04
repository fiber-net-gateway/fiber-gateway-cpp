#include "ProxyExecutor.h"
#include "../observability/AccessRequestTelemetry.h"

#include "../../../../src/async/TaskSelect.h"
#include "../../../../src/async/WhenAny.h"
#include "../../../../src/http/HttpBodySpec.h"
#include "../../../../src/http/HttpExchange.h"
#include "../../../../src/http/HttpHeaderHash.h"
#include "../../../../src/http/HttpHeaders.h"
#include "../../../../src/http/HttpProxyCore.h"
#include "ProxyResponsePlan.h"

#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace fiber::access_server {
namespace {

constexpr std::string_view kTraceId = "unknown-trace-id";

AccessError http_client_error(int status, std::string_view name, std::string_view message) {
    return AccessError{
            .status = status,
            .name = std::string(name),
            .message = std::string(message),
    };
}

AccessError map_request_error(const ProxyRequestError &request_error) {
    switch (request_error.code) {
        case ProxyRequestErrorCode::ResolveUpstream:
            return http_client_error(503, "HTTP_CLIENT_DNS_ERROR", "dns resolve error");
        case ProxyRequestErrorCode::SelectUpstream:
        case ProxyRequestErrorCode::PoolShutdown:
        case ProxyRequestErrorCode::Connect:
            return http_client_error(502, "HTTP_CLIENT_CONNECT_ERROR", "cannot connect to upstream");
        case ProxyRequestErrorCode::SendHeader:
        case ProxyRequestErrorCode::SendRequestBody:
            return http_client_error(502, "SEND_REQUEST_ERROR", "send request error");
        case ProxyRequestErrorCode::ReadResponseHeader:
            if (request_error.io_error == common::IoErr::TimedOut) {
                return http_client_error(504, "REQUEST_TIMEOUT", "request timeout");
            }
            return http_client_error(502, "HTTP_NO_RESPONSE", "no response");
        case ProxyRequestErrorCode::RequestBodyTooLarge:
            return AccessError::request_body_too_large();
        case ProxyRequestErrorCode::ReadRequestBody:
            return http_client_error(500, "HTTP_CLIENT_ERROR_BODY_SIZE",
                                     "predicated content length is not matched actual content length");
        case ProxyRequestErrorCode::BuildHeaders:
            return AccessError::unknown(request_error.message ? request_error.message : "proxy request failed");
    }
    return AccessError::unknown();
}

AccessError response_body_too_large(std::size_t size, bool chunked) {
    std::string message = chunked ? "chunked body size is too big：" : "body size is too big：";
    message.append(std::to_string(size));
    return http_client_error(500, "READ_RESP_BODY", message);
}

bool parse_content_length(std::string_view value, std::size_t &output) noexcept {
    if (value.empty()) {
        return false;
    }
    std::size_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return false;
    }
    output = result;
    return true;
}

bool apply_base_headers(http::HttpHeaders &output, std::span<const EvaluatedHeader> base_headers) noexcept {
    for (const EvaluatedHeader &header: base_headers) {
        if (!output.set(header.name, header.value)) {
            return false;
        }
    }
    return true;
}

bool build_downstream_headers(http::HttpExchange &downstream, const PreparedProxyRequest &request,
                              const ProxyUpstreamResponse &upstream, std::span<const EvaluatedHeader> base_headers,
                              std::span<const EvaluatedHeader> custom_headers, bool websocket_response,
                              http::HttpHeaders &output) {
    if (!apply_base_headers(output, base_headers)) {
        return false;
    }

    const http::Http1ResponseHead &head = upstream.head();
    for (const http::HttpHeaders::HeaderField &field: head.headers) {
        if (field.name_len == 0 || is_java_filtered_response_header(field.name_view()) ||
            request.response_headers.get().contains(field.lowcase_view(), field.name_hash)) {
            continue;
        }
        if (!output.add_view(field.name_view(), field.value_view(), field.lowcase_name, field.name_hash)) {
            return false;
        }
    }
    for (const EvaluatedHeader &header: custom_headers) {
        if (!output.add(header.name, header.value)) {
            return false;
        }
    }

    if (!request.response_headers.get().contains("Location")) {
        const std::string_view location = head.headers.get("Location");
        if (!location.empty()) {
            auto rewritten =
                    rewrite_java_proxy_location(location, upstream.endpoint().host_header,
                                                downstream.header("X-Forwarded-Proto"), downstream.header("Host"));
            if (rewritten && !output.set("Location", *rewritten)) {
                return false;
            }
        }
    }
    if (!request.response_headers.get().contains("Refresh")) {
        const std::string_view refresh = head.headers.get("Refresh");
        if (!refresh.empty()) {
            auto rewritten =
                    rewrite_java_proxy_refresh(refresh, upstream.endpoint().host_header,
                                               downstream.header("X-Forwarded-Proto"), downstream.header("Host"));
            if (rewritten && !output.set("Refresh", *rewritten)) {
                return false;
            }
        }
    }

    if (!websocket_response) {
        const std::string_view content_length = head.headers.get("Content-Length");
        if (!content_length.empty() && !output.set("Content-Length", content_length)) {
            return false;
        }
        if (request.flush && !output.set("X-Accel-Buffering", "no")) {
            return false;
        }
    }
    return true;
}

bool response_limit_exceeded(const PreparedProxyRequest &request, std::size_t size) noexcept {
    return request.max_response_body_size && *request.max_response_body_size > 0 &&
           size > *request.max_response_body_size;
}

} // namespace

ProxyExecutor::ProxyExecutor(ProxyRequestSender &sender, ProxyExecutorOptions options) noexcept :
    sender_(&sender), options_(options), error_responder_(options.error) {
    if (options_.response_body_chunk_size == 0) {
        options_.response_body_chunk_size = 64 * 1024;
    }
}

AccessProxyAdapter ProxyExecutor::adapter() noexcept {
    return AccessProxyAdapter{
            .context = this,
            .execute = execute_adapter,
    };
}

async::Task<common::IoResult<void>> ProxyExecutor::execute_adapter(void *context, http::HttpExchange &exchange,
                                                                   const PreparedProxyRequest &request,
                                                                   std::span<const EvaluatedHeader> base_headers,
                                                                   AccessRequestTelemetry *telemetry) noexcept {
    co_return co_await static_cast<ProxyExecutor *>(context)->execute(exchange, request, base_headers, telemetry);
}

async::Task<common::IoResult<void>> ProxyExecutor::execute(http::HttpExchange &exchange,
                                                           const PreparedProxyRequest &request,
                                                           std::span<const EvaluatedHeader> base_headers,
                                                           AccessRequestTelemetry *telemetry) noexcept {
    if (exchange.response_channel_closed()) {
        co_return common::IoResult<void>{};
    }

    auto completed = co_await async::when_any(
            [&exchange]() { return exchange.wait_response_channel_closed(); },
            [&]() { return execute_monitored(exchange, request, base_headers, telemetry).select(); });
    if (completed.is<1>()) {
        co_return std::move(completed).get<1>();
    }

    auto closed = std::move(completed).get<0>();
    if (!closed && !exchange.response_channel_closed()) {
        co_return std::unexpected(closed.error());
    }
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<void>> ProxyExecutor::execute_monitored(http::HttpExchange &exchange,
                                                                     const PreparedProxyRequest &request,
                                                                     std::span<const EvaluatedHeader> base_headers,
                                                                     AccessRequestTelemetry *telemetry) noexcept {
    auto started = co_await sender_->start(exchange, request, telemetry);
    if (!started) {
        co_return co_await error_responder_.send(
                exchange, map_request_error(started.error()), base_headers, {},
                telemetry && !telemetry->trace_id().empty() ? telemetry->trace_id() : kTraceId, true, telemetry);
    }
    ProxyUpstreamResponse upstream = std::move(*started);

    const bool websocket_response = request.websocket_upgrade && upstream.status_code() == 101;

    auto custom_headers = prepare_proxy_response_headers(request.response_headers.get(), request.response_evaluator);
    if (!custom_headers) {
        if (websocket_response) {
            (void) upstream.abort(common::IoErr::Canceled);
        } else {
            auto discarded = co_await upstream.discard_body();
            if (!discarded) {
                (void) upstream.abort(discarded.error());
            }
        }
        co_return co_await error_responder_.send(
                exchange, custom_headers.error(), base_headers, {},
                telemetry && !telemetry->trace_id().empty() ? telemetry->trace_id() : kTraceId, true, telemetry);
    }

    const http::Http1ResponseHead &upstream_head = upstream.head();
    const std::string_view content_length_text = upstream_head.headers.get("Content-Length");
    std::size_t content_length = 0;
    const bool has_content_length = parse_content_length(content_length_text, content_length);
    if (!websocket_response && has_content_length && response_limit_exceeded(request, content_length)) {
        (void) upstream.abort(common::IoErr::MessageTooLarge);
        co_return co_await error_responder_.send(
                exchange, response_body_too_large(content_length, false), base_headers, {},
                telemetry && !telemetry->trace_id().empty() ? telemetry->trace_id() : kTraceId, true, telemetry);
    }

    http::HttpHeaders response_headers(exchange.pool());
    if (!build_downstream_headers(exchange, request, upstream, base_headers, *custom_headers, websocket_response,
                                  response_headers) ||
        (telemetry && !telemetry->inject_response_headers(response_headers))) {
        (void) upstream.abort(common::IoErr::NoMem);
        co_return co_await error_responder_.send(
                exchange, AccessError::unknown("failed to build proxy response"), base_headers, {},
                telemetry && !telemetry->trace_id().empty() ? telemetry->trace_id() : kTraceId, true, telemetry);
    }

    const bool no_body = http::proxy_core::response_has_no_body(request.method, upstream_head.status_code);
    if (websocket_response) {
        auto switched = upstream.switch_to_raw_stream();
        const std::string_view upgrade = upstream_head.headers.get("Upgrade");
        if (!switched || !response_headers.set("Connection", "Upgrade") || !response_headers.set("Upgrade", upgrade)) {
            const common::IoErr error = switched ? common::IoErr::NoMem : switched.error();
            (void) upstream.abort(error);
            co_return co_await error_responder_.send(
                    exchange, AccessError::unknown("failed to upgrade websocket"), base_headers, {},
                    telemetry && !telemetry->trace_id().empty() ? telemetry->trace_id() : kTraceId, true, telemetry);
        }

        auto sent_header = co_await exchange.send_header(
                {
                        .kind = http::OutgoingHeaderKind::Final,
                        .status_code = 101,
                        .reason = upstream_head.reason,
                        .headers = &response_headers,
                        .body = http::HttpBodySpec::Stream(),
                        .connection_mode = http::ResponseConnectionMode::Auto,
                        .end_stream = false,
                },
                options_.downstream_write_timeout);
        if (!sent_header) {
            (void) upstream.abort(sent_header.error());
            co_return std::unexpected(sent_header.error());
        }

        co_await upstream.relay_websocket(exchange, std::chrono::milliseconds(request.websocket_timeout_millis));
        co_return common::IoResult<void>{};
    }

    const http::HttpBodySpec response_body =
            no_body ? http::HttpBodySpec::None()
                    : (has_content_length ? http::HttpBodySpec::ContentLength(content_length)
                                          : http::HttpBodySpec::Auto());
    const bool end_stream = no_body || (has_content_length && content_length == 0);
    auto sent_header = co_await exchange.send_header(
            {
                    .kind = http::OutgoingHeaderKind::Final,
                    .status_code = upstream_head.status_code,
                    .reason = upstream_head.reason,
                    .headers = &response_headers,
                    .body = response_body,
                    .connection_mode = http::ResponseConnectionMode::Auto,
                    .end_stream = end_stream,
            },
            options_.downstream_write_timeout);
    if (!sent_header) {
        (void) upstream.abort(sent_header.error());
        co_return std::unexpected(sent_header.error());
    }
    if (end_stream) {
        if (no_body) {
            auto discarded = co_await upstream.discard_body();
            if (!discarded) {
                (void) upstream.abort(discarded.error());
            }
        }
        co_return common::IoResult<void>{};
    }

    std::size_t received_body = 0;
    for (;;) {
        auto body = co_await upstream.read_body(options_.response_body_chunk_size);
        if (!body) {
            (void) exchange.abort(body.error());
            co_return std::unexpected(body.error());
        }
        const bool complete = body->complete();
        const std::size_t body_size = body->readable_bytes();
        if (body_size > std::numeric_limits<std::size_t>::max() - received_body) {
            (void) upstream.abort(common::IoErr::MessageTooLarge);
            (void) exchange.abort(common::IoErr::MessageTooLarge);
            co_return std::unexpected(common::IoErr::MessageTooLarge);
        }
        received_body += body_size;
        if (response_limit_exceeded(request, received_body)) {
            (void) upstream.abort(common::IoErr::MessageTooLarge);
            (void) exchange.abort(common::IoErr::MessageTooLarge);
            co_return std::unexpected(common::IoErr::MessageTooLarge);
        }

        auto written = co_await exchange.write_all(std::move(*body), options_.downstream_write_timeout);
        if (!written) {
            (void) upstream.abort(written.error());
            co_return std::unexpected(written.error());
        }
        if (*written != body_size) {
            (void) upstream.abort(common::IoErr::Invalid);
            co_return std::unexpected(common::IoErr::Invalid);
        }
        if (complete) {
            co_return common::IoResult<void>{};
        }
    }
}

} // namespace fiber::access_server
