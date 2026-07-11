#include "ProxyHandler.h"

#include <string>
#include <string_view>
#include <utility>

#include "common/IoError.h"
#include "event/EventLoop.h"
#include "http/ClientHttp1Exchange.h"
#include "http/Http1ClientConnection.h"
#include "http/HttpCommon.h"
#include "http/HttpExchange.h"
#include "http/HttpExchangeIo.h"
#include "http/HttpHeaderHash.h"
#include "http/HttpHeaders.h"

#include "../upstream/UpstreamRegistry.h"
#include "http/HttpProxyCore.h"

using namespace fiber::http::proxy_core;

namespace fiber::lite_nginx::proxy {
namespace {

using namespace fiber::http::proxy_core;

constexpr std::string_view kNotFoundBody = "404 Not Found\n";

bool should_skip_request_header(const runtime::LocationRuntime &location,
                                const fiber::http::HttpHeaders &request_headers,
                                const fiber::http::HttpHeaders::HeaderField &field) noexcept {
    if (location.skip_headers.get(field.lowcase_view(), field.name_hash)) {
        return true;
    }
    return connection_declares_hop_by_hop(request_headers, field.lowcase_view());
}

void apply_http3_alt_svc(const runtime::ListenerRuntime &listener, fiber::http::HttpHeaders &headers) {
    if (listener.http3 && !listener.http3_alt_svc.empty()) {
        headers.set("Alt-Svc", listener.http3_alt_svc);
    }
}

fiber::async::Task<void> send_plain_response(fiber::http::HttpExchange &exchange, int status_code,
                                             std::string_view body, const runtime::ListenerRuntime &listener) {
    fiber::http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", "text/plain");
    apply_http3_alt_svc(listener, headers);

    auto header_result = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = status_code,
            .headers = &headers,
            .body = fiber::http::HttpBodySpec::ContentLength(body.size()),
            .connection_mode = fiber::http::ResponseConnectionMode::Auto,
            .end_stream = body.empty(),
    });
    if (!header_result) {
        co_return;
    }
    if (body.empty()) {
        co_return;
    }

    (void) co_await exchange.write_body(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
}

void build_upstream_request_headers(const runtime::LocationRuntime &location, const fiber::http::HttpExchange &exchange,
                                    fiber::http::HttpHeaders &headers) {
    for (auto it = exchange.request_headers().begin(); it != exchange.request_headers().end(); ++it) {
        const auto &field = *it;
        if (field.name_len == 0 || should_skip_request_header(location, exchange.request_headers(), field)) {
            continue;
        }
        headers.add_view(field.name_view(), field.value_view(), const_cast<char *>(field.lowcase_name),
                         field.name_hash);
    }

    if (!location.host_header_overridden) {
        static constexpr std::uint64_t kHostHash = fiber::http::http_header_name_hash("host");
        headers.set_view("Host", location.default_host_header, const_cast<char *>("host"), kHostHash);
    }

    for (const auto &header: location.set_headers) {
        headers.set_view(header.name, header.value, const_cast<char *>(header.lowercase_name.data()), header.name_hash);
    }
}

void build_downstream_response_headers(const fiber::http::Http1ResponseHead &upstream_head,
                                       fiber::http::HttpHeaders &headers, const runtime::ListenerRuntime &listener) {
    for (auto it = upstream_head.headers.begin(); it != upstream_head.headers.end(); ++it) {
        const auto &field = *it;
        if (field.name_len == 0 || should_skip_hop_by_hop_header(upstream_head.headers, field)) {
            continue;
        }
        headers.add_view(field.name_view(), field.value_view(), const_cast<char *>(field.lowcase_name),
                         field.name_hash);
    }
    apply_http3_alt_svc(listener, headers);
}

fiber::async::Task<void> proxy_over_connection(fiber::http::HttpExchange &exchange,
                                               const runtime::LocationRuntime &location,
                                               fiber::http::Http1ClientConnection &conn,
                                               const runtime::ListenerRuntime &listener) {
    fiber::http::ClientHttp1Exchange upstream_exchange(conn, exchange.pool());
    fiber::http::HttpHeaders request_headers(exchange.pool());
    build_upstream_request_headers(location, exchange, request_headers);

    bool request_end_stream = true;
    const fiber::http::HttpBodySpec request_body = detect_request_body(exchange, request_end_stream);

    std::string target_scratch;
    fiber::http::Http1RequestHead request_head;
    request_head.method = exchange.method();
    request_head.target = request_target_view(exchange.uri(), target_scratch);
    request_head.headers = &request_headers;
    request_head.body = request_body;

    auto send_header_result =
            co_await upstream_exchange.send_header(request_head, request_end_stream, location.send_timeout);
    if (!send_header_result) {
        co_await send_plain_response(exchange, map_upstream_error_status(send_header_result.error()),
                                     map_upstream_error_body(send_header_result.error()), listener);
        co_return;
    }

    if (!request_end_stream) {
        while (true) {
            auto body_result = co_await exchange.read_body(kBodyChunkSize);
            if (!body_result) {
                co_return;
            }
            const bool last = body_result->complete();
            auto write_result = co_await upstream_exchange.write_body(std::move(*body_result), location.send_timeout);
            if (!write_result) {
                co_await send_plain_response(exchange, map_upstream_error_status(write_result.error()),
                                             map_upstream_error_body(write_result.error()), listener);
                co_return;
            }
            if (last) {
                break;
            }
        }
    }

    const fiber::http::Http1ResponseHead *upstream_head = nullptr;
    while (true) {
        auto read_header_result = co_await upstream_exchange.read_header(location.read_timeout);
        if (!read_header_result) {
            co_await send_plain_response(exchange, map_upstream_error_status(read_header_result.error()),
                                         map_upstream_error_body(read_header_result.error()), listener);
            co_return;
        }
        if (!(*read_header_result)->is_informational()) {
            upstream_head = *read_header_result;
            break;
        }
    }

    fiber::http::HttpHeaders response_headers(exchange.pool());
    build_downstream_response_headers(*upstream_head, response_headers, listener);

    const bool no_response_body = response_has_no_body(exchange.method(), upstream_head->status_code);
    std::size_t response_content_length = 0;
    const bool has_content_length =
            !no_response_body && parse_decimal(upstream_head->headers.get("content-length"), response_content_length);
    const fiber::http::HttpBodySpec response_body =
            no_response_body ? fiber::http::HttpBodySpec::None()
                             : (has_content_length ? fiber::http::HttpBodySpec::ContentLength(response_content_length)
                                                   : fiber::http::HttpBodySpec::Chunked());
    auto response_header_result = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = upstream_head->status_code,
            .reason = upstream_head->reason,
            .headers = &response_headers,
            .body = response_body,
            .connection_mode = fiber::http::ResponseConnectionMode::Auto,
            .end_stream = no_response_body || (has_content_length && response_content_length == 0),
    });
    if (!response_header_result) {
        co_return;
    }

    if (no_response_body) {
        (void) co_await upstream_exchange.discard_response_body(location.read_timeout);
        co_return;
    }

    while (true) {
        auto body_result = co_await upstream_exchange.read_body(kBodyChunkSize, location.read_timeout);
        if (!body_result) {
            co_return;
        }
        const bool last = body_result->complete();
        auto write_result = co_await exchange.write_body(std::move(*body_result));
        if (!write_result) {
            co_return;
        }
        if (last) {
            break;
        }
    }
}

struct TimeRec {
    std::chrono::steady_clock::time_point s;
    explicit TimeRec() noexcept { s = event::EventLoop::current().now(); }
    ~TimeRec() {
        const auto p = fiber::event::EventLoop::current().now() - s;
        std::fprintf(stderr, "cost: %llu\n", std::chrono::duration_cast<std::chrono::nanoseconds>(p).count());
    }
};

} // namespace

fiber::async::Task<void> ProxyHandler::handle(fiber::http::HttpExchange &exchange,
                                              const runtime::ListenerRuntime &listener,
                                              const runtime::LocationRuntime &location) const {
    if (!upstreams_) {
        co_await send_plain_response(exchange, 502, kBadGatewayBody, listener);
        co_return;
    }
    TimeRec rec;

    auto handle = co_await upstreams_->acquire_connection(location.upstream_index);
    if (!handle.valid()) {
        co_await send_plain_response(exchange, 502, kBadGatewayBody, listener);
        co_return;
    }

    fiber::http::Http1ClientConnectionOptions connection_options;
    connection_options.peer_addr = handle.peer_addr;
    connection_options.connect_timeout = location.connect_timeout;

    if (handle.pooled()) {
        fiber::http::Http1ClientConnection *conn = handle.lease.get();
        if (!conn) {
            auto emplace_result = handle.lease.emplace_connection(std::move(connection_options));
            if (!emplace_result) {
                co_await send_plain_response(exchange, map_upstream_error_status(emplace_result.error()),
                                             map_upstream_error_body(emplace_result.error()), listener);
                co_return;
            }
            conn = *emplace_result;
            auto connect_result = co_await conn->connect();
            if (!connect_result) {
                co_await send_plain_response(exchange, map_upstream_error_status(connect_result.error()),
                                             map_upstream_error_body(connect_result.error()), listener);
                co_return;
            }
        }
        co_await proxy_over_connection(exchange, location, *conn, listener);
        co_return;
    }

    fiber::http::Http1ClientConnection transient(fiber::event::EventLoop::current(), std::move(connection_options));
    auto connect_result = co_await transient.connect();
    if (!connect_result) {
        co_await send_plain_response(exchange, map_upstream_error_status(connect_result.error()),
                                     map_upstream_error_body(connect_result.error()), listener);
        co_return;
    }

    co_await proxy_over_connection(exchange, location, transient, listener);
}

} // namespace fiber::lite_nginx::proxy
