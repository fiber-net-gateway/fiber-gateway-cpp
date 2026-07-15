#include "ProxyHandler.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/IoError.h"
#include "http/ClientHttp1Exchange.h"
#include "http/Http1ClientConnection.h"
#include "http/HttpCommon.h"
#include "http/HttpExchange.h"
#include "http/HttpExchangeIo.h"
#include "http/HttpHeaderHash.h"
#include "http/HttpHeaders.h"
#include "http_script/ScriptExchangeCtx.h"
#include "script/JsValue.h"
#include "script/Script.h"
#include "script/std/NodeText.h"

#include "../runtime/DnsService.h"
#include "../upstream/ConnectionPool.h"
#include "../upstream/UpstreamConnection.h"
#include "../upstream/UpstreamRegistry.h"
#include "http/HttpProxyCore.h"

using namespace fiber::http::proxy_core;

namespace fiber::lite_nginx::proxy {
namespace {

using namespace fiber::http::proxy_core;

constexpr std::string_view kNotFoundBody = "404 Not Found\n";
constexpr std::string_view kScriptErrorBody = "500 Internal Server Error\n";

bool location_has_template_headers(const runtime::LocationRuntime &location) noexcept {
    for (const auto &header: location.set_headers) {
        if (header.template_script) {
            return true;
        }
    }
    return false;
}

// Evaluates all ${...} template header values for this request into `resolved` (index-aligned
// with location.set_headers; only template entries are filled). Returns false on abort /
// exception / non-String so the caller can respond 500. Builds a per-request GcHeap +
// ScriptExchangeCtx (route vars + services attached) only when the location has templates.
bool evaluate_template_headers(fiber::http::HttpExchange &exchange, const runtime::LocationRuntime &location,
                               const std::vector<std::pair<std::string_view, std::string_view>> &path_vars,
                               fiber::http_script::HttpScriptServices *services, std::vector<std::string> &resolved) {
    fiber::script::GcHeap heap;
    fiber::http_script::ScriptExchangeCtx ctx{exchange, heap};
    ctx.set_path_vars(path_vars);
    ctx.set_services(services);
    resolved.resize(location.set_headers.size());
    for (std::size_t i = 0; i < location.set_headers.size(); ++i) {
        const auto &header = location.set_headers[i];
        if (!header.template_script) {
            continue;
        }
        auto result = header.template_script->exec_sync(fiber::script::JsValue::make_undefined(), &ctx, heap);
        if (!result.is_success()) {
            return false;
        }
        std::string_view view;
        if (!fiber::script::std_lib::string_utf8_view(result.value(), view)) {
            return false; // non-String result
        }
        resolved[i].assign(view.data(), view.size());
    }
    return true;
}

bool should_skip_request_header(const runtime::LocationRuntime &location,
                                const fiber::http::HttpHeaders &request_headers,
                                const fiber::http::HttpHeaders::HeaderField &field) noexcept {
    if (is_request_framing_header(field) || location.skip_headers.get(field.lowcase_view(), field.name_hash)) {
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
                                    fiber::http::HttpHeaders &headers,
                                    const std::vector<std::string> &resolved_template_values) {
    for (auto it = exchange.request_headers().begin(); it != exchange.request_headers().end(); ++it) {
        const auto &field = *it;
        if (field.name_len == 0 || should_skip_request_header(location, exchange.request_headers(), field)) {
            continue;
        }
        headers.add_view(field.name_view(), field.value_view(), field.lowcase_name, field.name_hash);
    }

    if (!location.host_header_overridden) {
        static constexpr std::uint64_t kHostHash = fiber::http::http_header_name_hash("host");
        headers.set_view("Host", location.default_host_header, "host", kHostHash);
    }

    for (std::size_t i = 0; i < location.set_headers.size(); ++i) {
        const auto &header = location.set_headers[i];
        // Templates are pre-evaluated per request (resolved_template_values[i]); literals are
        // copied as-is. The ternary short-circuits, so resolved_template_values[i] is only
        // accessed for template entries (which are always filled when the location has templates).
        std::string_view value = header.template_script ? std::string_view(resolved_template_values[i]) : header.value;
        headers.set_view(header.name, value, header.lowercase_name.data(), header.name_hash);
    }
    remove_request_framing_headers(headers);
}

void build_downstream_response_headers(const fiber::http::Http1ResponseHead &upstream_head,
                                       fiber::http::HttpHeaders &headers, const runtime::ListenerRuntime &listener) {
    for (auto it = upstream_head.headers.begin(); it != upstream_head.headers.end(); ++it) {
        const auto &field = *it;
        if (field.name_len == 0 || should_skip_hop_by_hop_header(upstream_head.headers, field)) {
            continue;
        }
        headers.add_view(field.name_view(), field.value_view(), field.lowcase_name, field.name_hash);
    }
    apply_http3_alt_svc(listener, headers);
}

fiber::async::Task<void> proxy_over_connection(fiber::http::HttpExchange &exchange,
                                               const runtime::LocationRuntime &location,
                                               fiber::http::Http1ClientConnection &conn,
                                               const runtime::ListenerRuntime &listener,
                                               const std::vector<std::string> &resolved_template_values) {
    fiber::http::ClientHttp1Exchange upstream_exchange(conn, exchange.pool());
    fiber::http::HttpHeaders request_headers(exchange.pool());
    build_upstream_request_headers(location, exchange, request_headers, resolved_template_values);

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
    static constexpr std::uint64_t kContentLengthHash = fiber::http::http_header_name_hash("content-length");
    const bool has_content_length =
            !no_response_body &&
            parse_decimal(upstream_head->headers.get("content-length", kContentLengthHash), response_content_length);
    const fiber::http::HttpBodySpec response_body =
            no_response_body ? fiber::http::HttpBodySpec::None()
                             : (has_content_length ? fiber::http::HttpBodySpec::ContentLength(response_content_length)
                                                   : fiber::http::HttpBodySpec::Auto());
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

} // namespace

ProxyHandler::ProxyHandler(upstream::UpstreamRegistry &upstreams, upstream::ConnectionPool &pool,
                           runtime::DnsService &dns) noexcept : upstreams_(&upstreams), pool_(&pool), dns_(&dns) {}

fiber::async::Task<void>
ProxyHandler::handle(fiber::http::HttpExchange &exchange, const runtime::ListenerRuntime &listener,
                     const runtime::LocationRuntime &location,
                     const std::vector<std::pair<std::string_view, std::string_view>> &path_vars,
                     fiber::http_script::HttpScriptServices *services) const {
    if (!upstreams_ || !pool_) {
        co_await send_plain_response(exchange, 502, kBadGatewayBody, listener);
        co_return;
    }

    const auto *peer = upstreams_->select_peer(location.upstream_index);
    if (peer == nullptr || !peer->connection_key.has_value()) {
        co_await send_plain_response(exchange, 502, kBadGatewayBody, listener);
        co_return;
    }

    // Evaluate ${...} template header values before acquiring the upstream connection so a
    // template failure returns 500 without wasting a connection checkout. Static-header
    // locations skip this entirely (no GcHeap / ScriptExchangeCtx).
    std::vector<std::string> resolved_template_values;
    if (location_has_template_headers(location)) {
        if (!evaluate_template_headers(exchange, location, path_vars, services, resolved_template_values)) {
            co_await send_plain_response(exchange, 500, kScriptErrorBody, listener);
            co_return;
        }
    }

    // SNI uses the configured host name for HTTPS peers; IP-literal peers send nothing.
    const std::string_view sni =
            peer->connection_key->is_name() ? peer->connection_key->host_name() : std::string_view{};
    auto acquired =
            co_await upstream::acquire_and_connect(*pool_, *dns_, *peer->connection_key, sni, location.connect_timeout);
    if (!acquired) {
        co_await send_plain_response(exchange, map_upstream_error_status(acquired.error()),
                                     map_upstream_error_body(acquired.error()), listener);
        co_return;
    }

    co_await proxy_over_connection(exchange, location, *acquired->conn, listener, resolved_template_values);
}

} // namespace fiber::lite_nginx::proxy
