#include "ProxyHandler.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "common/IoError.h"
#include "event/EventLoop.h"
#include "http/ClientHttp1Exchange.h"
#include "http/HttpCommon.h"
#include "http/HttpExchange.h"
#include "http/HttpExchangeIo.h"
#include "http/HttpHeaderHash.h"
#include "http/HttpHeaders.h"
#include "http/Http1ClientConnection.h"

#include "../upstream/UpstreamRegistry.h"

namespace fiber::lite_nginx::proxy {
namespace {

constexpr std::string_view kNotFoundBody = "404 Not Found\n";
constexpr std::string_view kBadGatewayBody = "502 Bad Gateway\n";
constexpr std::string_view kGatewayTimeoutBody = "504 Gateway Timeout\n";
constexpr std::size_t kBodyChunkSize = 64 * 1024;

bool parse_decimal(std::string_view text, std::size_t &value) {
    if (text.empty()) {
        return false;
    }
    std::size_t out = 0;
    for (const unsigned char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        out = out * 10 + static_cast<std::size_t>(ch - '0');
    }
    value = out;
    return true;
}

bool is_hop_by_hop(std::string_view lowcase_name, std::uint64_t hash) {
    return (hash == fiber::http::http_header_name_hash("connection") && lowcase_name == "connection") ||
           (hash == fiber::http::http_header_name_hash("keep-alive") && lowcase_name == "keep-alive") ||
           (hash == fiber::http::http_header_name_hash("proxy-connection") && lowcase_name == "proxy-connection") ||
           (hash == fiber::http::http_header_name_hash("transfer-encoding") && lowcase_name == "transfer-encoding") ||
           (hash == fiber::http::http_header_name_hash("upgrade") && lowcase_name == "upgrade") ||
           (hash == fiber::http::http_header_name_hash("te") && lowcase_name == "te") ||
           (hash == fiber::http::http_header_name_hash("trailer") && lowcase_name == "trailer");
}

fiber::async::Task<void> send_plain_response(fiber::http::HttpExchange &exchange,
                                             int status_code,
                                             std::string_view body) {
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", "text/plain");

    auto header_result = co_await exchange.send_header({
        .kind = fiber::http::OutgoingHeaderKind::Final,
        .status_code = status_code,
        .headers = &headers,
        .body_mode = fiber::http::ResponseBodyMode::ContentLength,
        .connection_mode = fiber::http::ResponseConnectionMode::Auto,
        .content_length = body.size(),
        .end_stream = body.empty(),
    });
    if (!header_result) {
        co_return;
    }
    if (body.empty()) {
        co_return;
    }

    (void)co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
}

int map_upstream_error_status(fiber::common::IoErr err) noexcept {
    return err == fiber::common::IoErr::TimedOut ? 504 : 502;
}

std::string_view map_upstream_error_body(fiber::common::IoErr err) noexcept {
    return err == fiber::common::IoErr::TimedOut ? kGatewayTimeoutBody : kBadGatewayBody;
}

bool response_has_no_body(fiber::http::HttpMethod request_method, int status_code) noexcept {
    if (request_method == fiber::http::HttpMethod::Head) {
        return true;
    }
    return (status_code >= 100 && status_code < 200) || status_code == 204 || status_code == 304;
}

std::string_view request_target_view(const fiber::http::HttpUri &uri, std::string &scratch) {
    if (!uri.unparsed_uri.empty()) {
        return uri.unparsed_uri;
    }
    if (uri.query.empty()) {
        return uri.path;
    }
    scratch.assign(uri.path.data(), uri.path.size());
    scratch.push_back('?');
    scratch.append(uri.query.data(), uri.query.size());
    return scratch;
}

fiber::http::Http1RequestBodyMode detect_request_body_mode(const fiber::http::HttpExchange &exchange,
                                                           std::size_t &content_length,
                                                           bool &end_stream) {
    content_length = 0;
    end_stream = true;

    const auto *content_length_header = exchange.request_header_refs().content_type;
    (void)content_length_header;
    std::string_view content_length_value = exchange.request_headers().get("content-length");
    if (!content_length_value.empty()) {
        if (!parse_decimal(content_length_value, content_length)) {
            content_length = 0;
        }
        end_stream = content_length == 0;
        return fiber::http::Http1RequestBodyMode::ContentLength;
    }
    if (exchange.request_headers().contains("transfer-encoding")) {
        end_stream = false;
        return fiber::http::Http1RequestBodyMode::Chunked;
    }
    return fiber::http::Http1RequestBodyMode::None;
}

void build_upstream_request_headers(const runtime::LocationRuntime &location,
                                    const fiber::http::HttpExchange &exchange,
                                    fiber::http::HttpHeaders &headers) {
    for (auto it = exchange.request_headers().begin(); it != exchange.request_headers().end(); ++it) {
        const auto &field = *it;
        if (field.name_len == 0 || is_hop_by_hop(field.lowcase_view(), field.name_hash)) {
            continue;
        }
        headers.add_view(field.name_view(), field.value_view(), const_cast<char *>(field.lowcase_name), field.name_hash);
    }

    if (!location.host_header_overridden) {
        static constexpr std::uint64_t kHostHash = fiber::http::http_header_name_hash("host");
        headers.set_view("Host",
                         location.default_host_header,
                         const_cast<char *>("host"),
                         kHostHash);
    }

    for (const auto &header : location.set_headers) {
        headers.set_view(header.name,
                         header.value,
                         const_cast<char *>(header.lowercase_name.data()),
                         header.name_hash);
    }
}

void build_downstream_response_headers(const fiber::http::Http1ResponseHead &upstream_head,
                                       fiber::http::HttpHeaders &headers) {
    for (auto it = upstream_head.headers.begin(); it != upstream_head.headers.end(); ++it) {
        const auto &field = *it;
        if (field.name_len == 0 || is_hop_by_hop(field.lowcase_view(), field.name_hash)) {
            continue;
        }
        headers.add_view(field.name_view(), field.value_view(), const_cast<char *>(field.lowcase_name), field.name_hash);
    }
}

fiber::async::Task<void> proxy_over_connection(fiber::http::HttpExchange &exchange,
                                               const runtime::LocationRuntime &location,
                                               fiber::http::Http1ClientConnection &conn) {
    fiber::http::Http1ClientExchangeOptions exchange_options;
    exchange_options.write_timeout = location.send_timeout;
    exchange_options.response_header_timeout = location.read_timeout;
    exchange_options.response_body_timeout = location.read_timeout;

    fiber::http::ClientHttp1Exchange upstream_exchange(conn, exchange.pool(), exchange_options);
    fiber::http::HttpHeaders request_headers(exchange.pool());
    build_upstream_request_headers(location, exchange, request_headers);

    std::size_t request_content_length = 0;
    bool request_end_stream = true;
    const fiber::http::Http1RequestBodyMode request_body_mode =
        detect_request_body_mode(exchange, request_content_length, request_end_stream);

    std::string target_scratch;
    fiber::http::Http1RequestHead request_head;
    request_head.method = exchange.method();
    request_head.target = request_target_view(exchange.uri(), target_scratch);
    request_head.headers = &request_headers;
    request_head.body_mode = request_body_mode;
    request_head.content_length = request_content_length;

    auto send_header_result = co_await upstream_exchange.send_header(request_head, request_end_stream);
    if (!send_header_result) {
        co_await send_plain_response(exchange, map_upstream_error_status(send_header_result.error()),
                                     map_upstream_error_body(send_header_result.error()));
        co_return;
    }

    if (!request_end_stream) {
        while (true) {
            auto body_result = co_await exchange.read_body(kBodyChunkSize);
            if (!body_result) {
                co_return;
            }
            auto write_result = co_await upstream_exchange.write_body(std::move(*body_result));
            if (!write_result) {
                co_await send_plain_response(exchange, map_upstream_error_status(write_result.error()),
                                             map_upstream_error_body(write_result.error()));
                co_return;
            }
            if (body_result->last) {
                break;
            }
        }
    }

    const fiber::http::Http1ResponseHead *upstream_head = nullptr;
    while (true) {
        auto read_header_result = co_await upstream_exchange.read_header();
        if (!read_header_result) {
            co_await send_plain_response(exchange, map_upstream_error_status(read_header_result.error()),
                                         map_upstream_error_body(read_header_result.error()));
            co_return;
        }
        if (!(*read_header_result)->is_informational()) {
            upstream_head = *read_header_result;
            break;
        }
    }

    fiber::http::HttpHeaders response_headers(exchange.pool());
    build_downstream_response_headers(*upstream_head, response_headers);

    const bool no_response_body = response_has_no_body(exchange.method(), upstream_head->status_code);
    std::size_t response_content_length = 0;
    const bool has_content_length =
        !no_response_body && parse_decimal(upstream_head->headers.get("content-length"), response_content_length);
    auto response_header_result = co_await exchange.send_header({
        .kind = fiber::http::OutgoingHeaderKind::Final,
        .status_code = upstream_head->status_code,
        .reason = upstream_head->reason,
        .headers = &response_headers,
        .body_mode = has_content_length ? fiber::http::ResponseBodyMode::ContentLength
                                        : fiber::http::ResponseBodyMode::Chunked,
        .connection_mode = fiber::http::ResponseConnectionMode::Auto,
        .content_length = has_content_length ? response_content_length : 0,
        .end_stream = no_response_body || (has_content_length && response_content_length == 0),
    });
    if (!response_header_result) {
        co_return;
    }

    if (no_response_body) {
        (void)co_await upstream_exchange.discard_response_body();
        co_return;
    }

    while (true) {
        auto body_result = co_await upstream_exchange.read_body(kBodyChunkSize);
        if (!body_result) {
            co_return;
        }
        const bool last = body_result->last;
        auto write_result = co_await exchange.write_body(std::move(*body_result));
        if (!write_result) {
            co_return;
        }
        if (last) {
            break;
        }
    }
}

} // namespace

fiber::async::Task<void> ProxyHandler::handle(fiber::http::HttpExchange &exchange,
                                              const runtime::LocationRuntime &location) const {
    if (!upstreams_) {
        co_await send_plain_response(exchange, 502, kBadGatewayBody);
        co_return;
    }

    auto handle = upstreams_->acquire_connection(location.upstream_index);
    if (!handle.valid()) {
        co_await send_plain_response(exchange, 502, kBadGatewayBody);
        co_return;
    }

    fiber::http::Http1ClientConnectionOptions connection_options;
    connection_options.peer_addr = handle.peer->address;
    connection_options.connect_timeout = location.connect_timeout;

    if (handle.pooled()) {
        fiber::http::Http1ClientConnection *conn = handle.lease.get();
        if (!conn) {
            auto emplace_result = handle.lease.emplace_connection(std::move(connection_options));
            if (!emplace_result) {
                co_await send_plain_response(exchange, map_upstream_error_status(emplace_result.error()),
                                             map_upstream_error_body(emplace_result.error()));
                co_return;
            }
            conn = *emplace_result;
            auto connect_result = co_await conn->connect();
            if (!connect_result) {
                co_await send_plain_response(exchange, map_upstream_error_status(connect_result.error()),
                                             map_upstream_error_body(connect_result.error()));
                co_return;
            }
        }
        co_await proxy_over_connection(exchange, location, *conn);
        co_return;
    }

    fiber::http::Http1ClientConnection transient(fiber::event::EventLoop::current(), std::move(connection_options));
    auto connect_result = co_await transient.connect();
    if (!connect_result) {
        co_await send_plain_response(exchange, map_upstream_error_status(connect_result.error()),
                                     map_upstream_error_body(connect_result.error()));
        co_return;
    }

    co_await proxy_over_connection(exchange, location, transient);
}

} // namespace fiber::lite_nginx::proxy
