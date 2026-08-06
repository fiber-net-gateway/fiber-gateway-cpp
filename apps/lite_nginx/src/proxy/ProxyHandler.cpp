#include "ProxyHandler.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "async/TaskSelect.h"
#include "async/WhenAny.h"
#include "common/IoError.h"
#include "event/EventLoop.h"
#include "http/ClientHttp1Exchange.h"
#include "http/Http1ClientConnection.h"
#include "http/HttpBodyPipe.h"
#include "http/HttpCommon.h"
#include "http/HttpExchange.h"
#include "http/HttpExchangeIo.h"
#include "http/HttpHeaderHash.h"
#include "http/HttpHeaders.h"
#include "http/HttpWebSocketProxy.h"
#include "http_script/ConstPackage.h"
#include "http_script/ScriptExchangeCtx.h"
#include "log/Log.h"
#include "script/JsValue.h"
#include "script/Script.h"
#include "script/std/NodeText.h"

#include "../logging/AccessLogger.h"
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
constexpr std::string_view kNotImplementedBody = "501 Not Implemented\n";

DEFINE_LOGGER(LOG_PROXY, "lite_nginx.proxy");

void record_upstream_error(logging::RequestLogContext &context, fiber::common::IoErr error,
                           std::string_view phase) noexcept {
    if (context.upstream_error == fiber::common::IoErr::None) {
        context.upstream_error = error;
    }
    LOG(LOG_PROXY, WARN) << "request_id=" << context.request_id << " upstream="
                         << fiber::log::quoted(context.upstream_host.empty() ? std::string_view("-")
                                                                             : context.upstream_host)
                         << " port=" << context.upstream_port << " phase=" << phase
                         << " error=" << fiber::common::io_err_name(error);
}

void record_client_abort(logging::RequestLogContext &context) noexcept {
    context.client_aborted = true;
    LOG(LOG_PROXY, INFO) << "request_id=" << context.request_id << " upstream="
                         << fiber::log::quoted(context.upstream_host.empty() ? std::string_view("-")
                                                                             : context.upstream_host)
                         << " port=" << context.upstream_port << " phase=client_abort";
}

struct ResolvedProxyValues {
    std::vector<std::string> header_values;
    std::string rewritten_path;
    std::string request_target_storage;
    std::string_view request_target;
};

bool location_has_proxy_templates(const runtime::LocationRuntime &location) noexcept {
    if (location.rewrite_path.kind == runtime::RewritePathKind::Template) {
        return true;
    }
    for (const auto &header: location.set_headers) {
        if (header.template_script) {
            return true;
        }
    }
    return false;
}

bool evaluate_proxy_templates(fiber::http::HttpExchange &exchange, const runtime::LocationRuntime &location,
                              const std::vector<std::pair<std::string_view, std::string_view>> &path_vars,
                              fiber::http_script::HttpScriptServices *services,
                              fiber::http_script::ScriptConnectionInfo connection, ResolvedProxyValues &resolved) {
    fiber::script::GcHeap heap(exchange.pool());
    fiber::http_script::ScriptExchangeCtx ctx{exchange, heap, connection};
    if (!location.const_package) {
        return false;
    }
    auto constants_ready = ctx.prepare_constants(*location.const_package);
    if (!constants_ready || !ctx.bind_path_constants(*location.const_package, path_vars)) {
        return false;
    }
    ctx.set_services(services);
    resolved.header_values.resize(location.set_headers.size());

    if (location.rewrite_path.kind == runtime::RewritePathKind::Template) {
        if (!location.rewrite_path.template_script) {
            return false;
        }
        auto result =
                location.rewrite_path.template_script->exec_sync(fiber::script::JsValue::make_undefined(), &ctx, heap);
        if (!result.is_success()) {
            return false;
        }
        std::string_view view;
        if (!fiber::script::std_lib::string_utf8_view(result.value(), view)) {
            return false;
        }
        resolved.rewritten_path.assign(view.data(), view.size());
    }

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
        resolved.header_values[i].assign(view.data(), view.size());
    }
    return true;
}

std::string_view raw_query_suffix(const fiber::http::HttpUri &uri) noexcept {
    if (!uri.unparsed_uri.empty()) {
        const std::size_t query = uri.unparsed_uri.find('?');
        if (query != std::string_view::npos) {
            const std::size_t fragment = uri.unparsed_uri.find('#', query + 1);
            const std::size_t end = fragment == std::string_view::npos ? uri.unparsed_uri.size() : fragment;
            return uri.unparsed_uri.substr(query, end - query);
        }
    }
    return {};
}

bool resolve_request_target(const fiber::http::HttpExchange &exchange, const runtime::LocationRuntime &location,
                            ResolvedProxyValues &resolved) {
    if (location.rewrite_path.kind == runtime::RewritePathKind::Preserve) {
        resolved.request_target = request_target_view(exchange.uri(), resolved.request_target_storage);
        return !resolved.request_target.empty();
    }

    const std::string_view path = location.rewrite_path.kind == runtime::RewritePathKind::Literal
                                          ? std::string_view(location.rewrite_path.literal)
                                          : std::string_view(resolved.rewritten_path);
    if (!valid_origin_form_path(path)) {
        return false;
    }

    std::string_view query_suffix = raw_query_suffix(exchange.uri());
    if (query_suffix.empty() && exchange.uri().query.empty()) {
        resolved.request_target = path;
        return true;
    }

    resolved.request_target_storage.assign(path.data(), path.size());
    if (!query_suffix.empty()) {
        resolved.request_target_storage.append(query_suffix.data(), query_suffix.size());
    } else if (!exchange.uri().query.empty()) {
        resolved.request_target_storage.push_back('?');
        resolved.request_target_storage.append(exchange.uri().query.data(), exchange.uri().query.size());
    }
    resolved.request_target = resolved.request_target_storage;
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

    (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
}

bool build_upstream_request_headers(const runtime::LocationRuntime &location, const fiber::http::HttpExchange &exchange,
                                    fiber::http::HttpHeaders &headers,
                                    const std::vector<std::string> &resolved_template_values,
                                    WebSocketHandshake &websocket) {
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
    if (websocket.active()) {
        return prepare_upstream_websocket_headers(exchange, websocket, headers);
    }
    if (!location.reuse_connection) {
        headers.set("Connection", "close");
    }
    return true;
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

fiber::async::Task<void>
proxy_over_connection(fiber::http::HttpExchange &exchange, const runtime::LocationRuntime &location,
                      fiber::http::Http1ClientConnection &conn, const runtime::ListenerRuntime &listener,
                      const std::vector<std::string> &resolved_template_values, std::string_view request_target,
                      logging::RequestLogContext &log_context) {
    WebSocketHandshake websocket{
            .downstream = detect_websocket_downstream(exchange),
    };
    if (!prepare_websocket_handshake(websocket)) {
        record_upstream_error(log_context, fiber::common::IoErr::Invalid, "websocket_request");
        co_await send_plain_response(exchange, 500, kScriptErrorBody, listener);
        co_return;
    }

    fiber::http::ClientHttp1Exchange upstream_exchange(conn, exchange.pool());
    fiber::http::HttpHeaders request_headers(exchange.pool());
    if (!build_upstream_request_headers(location, exchange, request_headers, resolved_template_values, websocket)) {
        co_await send_plain_response(exchange, 502, kBadGatewayBody, listener);
        co_return;
    }

    bool request_end_stream = true;
    const fiber::http::HttpBodySpec request_body =
            websocket.active() ? fiber::http::HttpBodySpec::None() : detect_request_body(exchange, request_end_stream);
    if (websocket.active()) {
        request_end_stream = true;
    }

    fiber::http::Http1RequestHead request_head;
    request_head.method = websocket.active() ? fiber::http::HttpMethod::Get : exchange.method();
    request_head.target = request_target;
    request_head.headers = &request_headers;
    request_head.body = request_body;

    auto send_header_result =
            co_await upstream_exchange.send_header(request_head, request_end_stream, location.send_timeout);
    if (!send_header_result) {
        record_upstream_error(log_context, send_header_result.error(), "send_header");
        co_await send_plain_response(exchange, map_upstream_error_status(send_header_result.error()),
                                     map_upstream_error_body(send_header_result.error()), listener);
        co_return;
    }

    if (!request_end_stream) {
        RequestBodyForwardState forward_state(request_body);
        while (true) {
            auto body_result = co_await exchange.read_body(kBodyChunkSize);
            if (!body_result) {
                (void) upstream_exchange.abort(body_result.error());
                co_return;
            }
            const bool last = body_result->complete();
            const std::size_t body_bytes = body_result->readable_bytes();
            if (!forward_state.accepts(body_bytes)) {
                (void) upstream_exchange.abort(fiber::common::IoErr::Invalid);
                (void) exchange.abort(fiber::common::IoErr::Invalid);
                co_return;
            }
            if (forward_state.should_write(body_bytes)) {
                auto write_result =
                        co_await upstream_exchange.write_all(std::move(*body_result), location.send_timeout);
                if (!write_result) {
                    record_upstream_error(log_context, write_result.error(), "send_body");
                    co_await send_plain_response(exchange, map_upstream_error_status(write_result.error()),
                                                 map_upstream_error_body(write_result.error()), listener);
                    co_return;
                }
                forward_state.record_write(*write_result);
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
            record_upstream_error(log_context, read_header_result.error(), "read_header");
            co_await send_plain_response(exchange, map_upstream_error_status(read_header_result.error()),
                                         map_upstream_error_body(read_header_result.error()), listener);
            co_return;
        }
        if ((*read_header_result)->status_code == 101 || !(*read_header_result)->is_informational()) {
            upstream_head = *read_header_result;
            break;
        }
    }

    log_context.upstream_status = upstream_head->status_code;

    if (upstream_head->status_code == 101) {
        if (!valid_websocket_upgrade_response(*upstream_head, websocket)) {
            record_upstream_error(log_context, fiber::common::IoErr::Invalid, "websocket_response");
            co_await send_plain_response(exchange, 502, kBadGatewayBody, listener);
            co_return;
        }
        auto switch_result = upstream_exchange.switch_to_raw_stream();
        if (!switch_result) {
            record_upstream_error(log_context, switch_result.error(), "websocket_switch");
            co_await send_plain_response(exchange, 502, kBadGatewayBody, listener);
            co_return;
        }

        fiber::http::HttpHeaders response_headers(exchange.pool());
        build_downstream_websocket_headers(*upstream_head, response_headers, websocket);
        apply_http3_alt_svc(listener, response_headers);
        auto response_header_result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = websocket.extended_connect() ? 200 : 101,
                .reason = websocket.extended_connect() ? std::string_view{} : upstream_head->reason,
                .headers = &response_headers,
                .body = fiber::http::HttpBodySpec::Stream(),
                .connection_mode = fiber::http::ResponseConnectionMode::Auto,
                .end_stream = false,
        });
        if (!response_header_result) {
            (void) upstream_exchange.abort(response_header_result.error());
            co_return;
        }

        co_await relay_websocket_tunnel(exchange, upstream_exchange, location.read_timeout, location.send_timeout);
        co_return;
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
        (void) upstream_exchange.abort(response_header_result.error());
        co_return;
    }

    if (no_response_body) {
        if (location.reuse_connection) {
            (void) co_await upstream_exchange.discard_response_body(location.read_timeout);
        }
        co_return;
    }
    if (has_content_length && response_content_length == 0) {
        co_return;
    }

    const fiber::http::HttpBodyPipeOptions pipe_options{
            .buffer_size = location.buffering.buffer_size,
            .low_water = location.buffering.low_water,
            .read_timeout = location.read_timeout,
            .write_timeout = location.send_timeout,
    };
    auto pipe_result =
            co_await fiber::http::pipe_http_body(fiber::http::make_http_body_pipe_reader(upstream_exchange),
                                                 fiber::http::make_http_body_pipe_writer(exchange),
                                                 fiber::event::EventLoop::current().io_buf_node_pool(), pipe_options);
    if (!pipe_result) {
        if (pipe_result.error().phase == fiber::http::HttpBodyPipePhase::Read) {
            record_upstream_error(log_context, pipe_result.error().code, "read_body");
        } else if (pipe_result.error().phase == fiber::http::HttpBodyPipePhase::Validate) {
            record_upstream_error(log_context, pipe_result.error().code, "body_pipe");
        }
        co_return;
    }
}

fiber::async::Task<void> run_proxy_request(fiber::http::HttpExchange &exchange,
                                           const runtime::ListenerRuntime &listener,
                                           const runtime::LocationRuntime &location,
                                           const std::vector<std::pair<std::string_view, std::string_view>> &path_vars,
                                           fiber::http_script::HttpScriptServices *services,
                                           logging::RequestLogContext &log_context, upstream::ConnectionPool &pool,
                                           runtime::DnsService &dns, const runtime::UpstreamPeerRuntime &peer,
                                           upstream::AcquiredUpstreamConnection &acquired) {
    ResolvedProxyValues resolved;
    if (location_has_proxy_templates(location) &&
        !evaluate_proxy_templates(exchange, location, path_vars, services, log_context.connection, resolved)) {
        co_await send_plain_response(exchange, 500, kScriptErrorBody, listener);
        co_return;
    }
    if (!resolve_request_target(exchange, location, resolved)) {
        co_await send_plain_response(exchange, 500, kScriptErrorBody, listener);
        co_return;
    }

    const std::string_view sni = peer.connection_key->is_name() ? peer.connection_key->host_name() : std::string_view{};
    const auto reuse_policy = location.reuse_connection ? upstream::ConnectionReusePolicy::Pooled
                                                        : upstream::ConnectionReusePolicy::Transient;
    auto acquired_result = co_await upstream::acquire_and_connect(pool, dns, *peer.connection_key, sni,
                                                                  location.connect_timeout, reuse_policy);
    if (!acquired_result) {
        record_upstream_error(log_context, acquired_result.error(), "connect");
        co_await send_plain_response(exchange, map_upstream_error_status(acquired_result.error()),
                                     map_upstream_error_body(acquired_result.error()), listener);
        co_return;
    }
    acquired = std::move(*acquired_result);

    co_await proxy_over_connection(exchange, location, *acquired.conn, listener, resolved.header_values,
                                   resolved.request_target, log_context);
}

} // namespace

ProxyHandler::ProxyHandler(upstream::UpstreamRegistry &upstreams, upstream::ConnectionPool &pool,
                           runtime::DnsService &dns) noexcept : upstreams_(&upstreams), pool_(&pool), dns_(&dns) {}

fiber::async::Task<void>
ProxyHandler::handle(fiber::http::HttpExchange &exchange, const runtime::ListenerRuntime &listener,
                     const runtime::LocationRuntime &location,
                     const std::vector<std::pair<std::string_view, std::string_view>> &path_vars,
                     fiber::http_script::HttpScriptServices *services, logging::RequestLogContext &log_context) const {
    if (!exchange.protocol().empty() && (exchange.method() != fiber::http::HttpMethod::Connect ||
                                         !fiber::http::http_header_name_equals_ci(exchange.protocol(), "websocket"))) {
        co_await send_plain_response(exchange, 501, kNotImplementedBody, listener);
        co_return;
    }

    if (!upstreams_ || !pool_) {
        record_upstream_error(log_context, fiber::common::IoErr::Invalid, "runtime");
        co_await send_plain_response(exchange, 502, kBadGatewayBody, listener);
        co_return;
    }

    const auto *peer = upstreams_->select_peer(location.upstream_index);
    if (peer == nullptr || !peer->connection_key.has_value()) {
        record_upstream_error(log_context, fiber::common::IoErr::NotFound, "select_peer");
        co_await send_plain_response(exchange, 502, kBadGatewayBody, listener);
        co_return;
    }
    log_context.upstream_host = peer->host;
    log_context.upstream_port = peer->port;
    log_context.upstream_started_at = fiber::event::EventLoop::current().now();
    log_context.upstream_started = true;

    upstream::AcquiredUpstreamConnection acquired;
    if (!location.close_on_client_abort) {
        co_await run_proxy_request(exchange, listener, location, path_vars, services, log_context, *pool_, *dns_, *peer,
                                   acquired);
        co_return;
    }

    auto completed = co_await fiber::async::when_any([&exchange]() { return exchange.wait_response_channel_closed(); },
                                                     [&]() {
                                                         return run_proxy_request(exchange, listener, location,
                                                                                  path_vars, services, log_context,
                                                                                  *pool_, *dns_, *peer, acquired)
                                                                 .select();
                                                     });
    if (completed.is<1>()) {
        completed.get<1>();
        co_return;
    }

    auto close_result = std::move(completed).get<0>();
    if (!close_result) {
        record_upstream_error(log_context, close_result.error(), "client_abort_monitor");
        if (!exchange.response_channel_closed()) {
            co_await send_plain_response(exchange, 500, kScriptErrorBody, listener);
        }
        co_return;
    }

    record_client_abort(log_context);
    if (acquired.conn != nullptr && acquired.conn->valid()) {
        acquired.conn->close();
    }
}

} // namespace fiber::lite_nginx::proxy
