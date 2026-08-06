#include "McpHttpHandler.h"

#include "../mcp/McpJsonCodec.h"
#include "../mcp/McpProtocol.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include <async/Sleep.h>
#include <async/WhenAny.h>
#include <common/IoError.h>
#include <common/mem/IoBufChain.h>
#include <common/util/UrlForm.h>
#include <event/EventLoop.h>
#include <http/HttpBodySpec.h>
#include <http/HttpCommon.h>
#include <http/HttpExchange.h>
#include <http/HttpHeaders.h>

namespace fiber::ai_server {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t kMaxRequestBytes = 4 * 1024 * 1024;
constexpr std::chrono::seconds kHeartbeatInitialDelay{20};
constexpr std::chrono::seconds kHeartbeatInterval{25};
constexpr std::string_view kSessionHeader = "mcp-session-id";
constexpr std::string_view kProtocolHeader = "mcp-protocol-version";

struct McpRoute {
    std::string_view project;
    std::string_view endpoint;
};

McpRoute parse_route(std::string_view path) noexcept {
    if (path.size() < 4 || path.front() != '/') {
        return {};
    }
    path.remove_prefix(1);
    const std::size_t slash = path.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= path.size()) {
        return {};
    }
    const std::string_view project = path.substr(0, slash);
    const std::string_view endpoint = path.substr(slash + 1);
    if (endpoint.find('/') != std::string_view::npos ||
        (endpoint != "mcp" && endpoint != "sse" && endpoint != "message")) {
        return {};
    }
    return {project, endpoint};
}

bool contains_token(std::string_view value, std::string_view token) noexcept {
    auto lower = [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); };
    return std::search(value.begin(), value.end(), token.begin(), token.end(),
                       [&](char left, char right) { return lower(left) == lower(right); }) != value.end();
}

async::Task<common::IoResult<std::string>> read_request_body(http::HttpExchange &exchange) noexcept {
    std::string body;
    body.reserve(1024);
    for (;;) {
        const std::size_t remaining = kMaxRequestBytes - body.size();
        auto chunk = co_await exchange.read_body(std::min<std::size_t>(64 * 1024, remaining + 1));
        if (!chunk) {
            co_return std::unexpected(chunk.error());
        }
        if (chunk->readable_bytes() > remaining) {
            co_return std::unexpected(common::IoErr::MessageTooLarge);
        }
        while (const mem::IoBuf *part = chunk->first_readable()) {
            const std::size_t size = part->readable();
            body.append(reinterpret_cast<const char *>(part->readable_data()), size);
            chunk->consume_and_compact(size);
        }
        if (chunk->complete()) {
            break;
        }
    }
    co_return body;
}

async::Task<void> send_body(http::HttpExchange &exchange, int status, std::string_view content_type,
                            std::string_view body, const McpSession *session = nullptr) noexcept {
    http::HttpHeaders headers(exchange.pool());
    headers.set("Content-Type", content_type);
    if (session) {
        headers.set(kSessionHeader, session->id());
    }
    auto sent = co_await exchange.send_header({
            .kind = http::OutgoingHeaderKind::Final,
            .status_code = status,
            .headers = &headers,
            .body = http::HttpBodySpec::ContentLength(body.size()),
            .connection_mode = http::ResponseConnectionMode::Auto,
            .end_stream = body.empty(),
    });
    if (sent && !body.empty()) {
        (void) co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(body.data()), body.size(), true);
    }
}

async::Task<void> send_error(http::HttpExchange &exchange, int status, int code, std::string_view message) noexcept {
    std::string body = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":";
    body.append(std::to_string(code));
    body.append(",\"message\":");
    (void) append_json_string(body, message);
    body.append("}}\n");
    co_await send_body(exchange, status, "application/json", body);
}

bool valid_protocol_header(http::HttpExchange &exchange) noexcept {
    const std::string_view version = exchange.header(kProtocolHeader);
    return version.empty() || McpProtocol::supported_version(version);
}

std::string make_sse_messages(const std::vector<std::string> &responses) {
    std::size_t capacity = 0;
    for (const std::string &response: responses) {
        capacity += response.size() + 23;
    }
    std::string body;
    body.reserve(capacity);
    for (const std::string &response: responses) {
        body.append("event: message\ndata: ");
        body.append(response);
        body.append("\n\n");
    }
    return body;
}

std::string query_session_id(std::string_view query) {
    std::string session_id;
    auto decoded = util::form_decode_query(query, [&](std::string_view key, std::string_view value) {
        if (key == "sessionId") {
            session_id.assign(value);
        }
        return true;
    });
    return decoded ? session_id : std::string{};
}

} // namespace

bool McpHttpHandler::matches(std::string_view path) const noexcept {
    const McpRoute route = parse_route(path);
    return !route.project.empty();
}

async::Task<void> McpHttpHandler::handle(http::HttpExchange &exchange) noexcept {
    const McpRoute route = parse_route(exchange.uri().path);
    if (route.project.empty()) {
        co_await send_error(exchange, 404, -32601, "MCP route not found");
        co_return;
    }
    const auto snapshot = config_->snapshot();
    auto project = snapshot ? snapshot->find_project_shared(route.project) : nullptr;
    if (!project) {
        co_await send_error(exchange, 404, -32601, "MCP project not found");
        co_return;
    }
    if (route.endpoint == "mcp") {
        co_await handle_streamable(exchange, route.project, std::move(project));
    } else if (route.endpoint == "sse") {
        co_await handle_legacy_sse(exchange, std::move(project));
    } else {
        co_await handle_legacy_message(exchange, route.project);
    }
}

async::Task<void> McpHttpHandler::handle_streamable(http::HttpExchange &exchange, std::string_view project_name,
                                                    std::shared_ptr<const McpProjectRuntime> project) noexcept {
    const std::string_view session_header = exchange.header(kSessionHeader);
    std::shared_ptr<McpSession> session;
    if (!session_header.empty()) {
        session = sessions_->find(session_header);
        if (!session) {
            const std::string_view prefix = sessions_->parse_prefix(session_header);
            if (!prefix.empty() && prefix != sessions_->local_prefix() && forwarder_) {
                const McpForwardResult result = co_await forwarder_->forward(exchange, prefix);
                if (result == McpForwardResult::Completed || exchange.response_stats().header_sent) {
                    co_return;
                }
                if (result == McpForwardResult::Failed) {
                    co_await send_error(exchange, 502, -32603, "MCP session owner is unavailable");
                    co_return;
                }
            }
            co_await send_error(exchange, 404, -32001, "Session ID not found");
            co_return;
        }
        if (session->transport() != McpTransport::StreamableHttp || session->project()->name != project_name) {
            co_await send_error(exchange, 400, -32000, "Session uses a different MCP route or transport");
            co_return;
        }
    }

    if (exchange.method() == http::HttpMethod::Post) {
        const std::string_view content_type = exchange.header("content-type");
        const std::string_view accept = exchange.header("accept");
        if (!contains_token(content_type, "application/json")) {
            co_await send_error(exchange, 415, -32000, "Content-Type must be application/json");
            co_return;
        }
        if (!contains_token(accept, "application/json") || !contains_token(accept, "text/event-stream")) {
            co_await send_error(exchange, 406, -32000, "Accept must include application/json and text/event-stream");
            co_return;
        }
        const bool created = !session;
        if (!session) {
            session = sessions_->create(McpTransport::StreamableHttp, std::move(project),
                                        event::EventLoop::current().now());
        }
        auto body = co_await read_request_body(exchange);
        if (!body) {
            if (created) {
                (void) sessions_->erase(session->id());
            }
            co_await send_error(exchange, body.error() == common::IoErr::MessageTooLarge ? 413 : 400, -32700,
                                "Invalid MCP request body");
            co_return;
        }
        auto output = co_await McpProtocol::process(exchange, session, *body, exchange.header(kProtocolHeader));
        if (!output) {
            if (created) {
                (void) sessions_->erase(session->id());
            }
            co_await send_error(exchange, output.error().http_status, output.error().json_rpc_code,
                                output.error().message);
            co_return;
        }
        if (created && session->state() == McpSessionState::Created) {
            (void) sessions_->erase(session->id());
            co_await send_error(exchange, 400, -32600, "First request must initialize the MCP session");
            co_return;
        }
        if (!output->has_request) {
            co_await send_body(exchange, 202, "application/json", {}, session.get());
            co_return;
        }
        const std::string response = make_sse_messages(output->responses);
        co_await send_body(exchange, 200, "text/event-stream", response, session.get());
        co_return;
    }

    if (!session || !session->validate_initialized() || !valid_protocol_header(exchange)) {
        co_await send_error(exchange, session ? 400 : 400, -32000, "Valid initialized MCP session is required");
        co_return;
    }
    if (exchange.method() == http::HttpMethod::Delete) {
        (void) sessions_->erase(session->id());
        co_await send_body(exchange, 200, "application/json", {});
        co_return;
    }
    if (exchange.method() != http::HttpMethod::Get) {
        co_await send_error(exchange, 405, -32000, "Method not allowed");
        co_return;
    }
    if (!contains_token(exchange.header("accept"), "text/event-stream")) {
        co_await send_error(exchange, 406, -32000, "Client must accept text/event-stream");
        co_return;
    }
    auto mailbox = std::make_shared<McpStreamMailbox>();
    if (!session->attach_stream(mailbox)) {
        co_await send_error(exchange, 409, -32000, "Only one SSE stream is allowed per session");
        co_return;
    }
    co_await stream(exchange, session, mailbox);
}

async::Task<void> McpHttpHandler::handle_legacy_sse(http::HttpExchange &exchange,
                                                    std::shared_ptr<const McpProjectRuntime> project) noexcept {
    if (exchange.method() != http::HttpMethod::Get) {
        co_await send_error(exchange, 405, -32000, "Method not allowed");
        co_return;
    }
    auto session = sessions_->create(McpTransport::LegacySse, std::move(project), event::EventLoop::current().now());
    auto mailbox = std::make_shared<McpStreamMailbox>();
    if (!session->attach_stream(mailbox)) {
        (void) sessions_->erase(session->id());
        co_await send_error(exchange, 500, -32603, "Failed to create MCP stream");
        co_return;
    }
    std::string endpoint;
    std::string_view proto = exchange.header("x-forwarded-proto");
    if (proto.empty()) {
        proto = "http";
    }
    const auto session_project = session->project();
    endpoint.reserve(proto.size() + exchange.header("host").size() + session_project->name.size() +
                     session->id().size() + 32);
    endpoint.append(proto);
    endpoint.append("://");
    endpoint.append(exchange.header("host"));
    endpoint.push_back('/');
    endpoint.append(session_project->name);
    endpoint.append("/message?sessionId=");
    endpoint.append(session->id());
    co_await stream(exchange, session, mailbox, std::move(endpoint));
    (void) sessions_->erase(session->id());
}

async::Task<void> McpHttpHandler::handle_legacy_message(http::HttpExchange &exchange,
                                                        std::string_view project_name) noexcept {
    if (exchange.method() != http::HttpMethod::Post) {
        co_await send_error(exchange, 405, -32000, "Method not allowed");
        co_return;
    }
    if (!contains_token(exchange.header("content-type"), "application/json")) {
        co_await send_error(exchange, 400, -32000, "Content-Type must be application/json");
        co_return;
    }
    const std::string session_id = query_session_id(exchange.uri().query);
    auto session = sessions_->find(session_id);
    if (!session) {
        const std::string_view prefix = sessions_->parse_prefix(session_id);
        if (!prefix.empty() && prefix != sessions_->local_prefix() && forwarder_) {
            const McpForwardResult result = co_await forwarder_->forward(exchange, prefix);
            if (result == McpForwardResult::Completed || exchange.response_stats().header_sent) {
                co_return;
            }
            if (result == McpForwardResult::Failed) {
                co_await send_error(exchange, 502, -32603, "MCP session owner is unavailable");
                co_return;
            }
        }
    }
    if (!session || session->transport() != McpTransport::LegacySse || session->project()->name != project_name) {
        co_await send_error(exchange, 404, -32001, "Session ID not found");
        co_return;
    }
    auto body = co_await read_request_body(exchange);
    if (!body) {
        co_await send_error(exchange, 400, -32700, "Invalid MCP request body");
        co_return;
    }
    auto output = co_await McpProtocol::process(exchange, session, *body);
    if (!output) {
        co_await send_error(exchange, output.error().http_status, output.error().json_rpc_code, output.error().message);
        co_return;
    }
    for (std::string &response: output->responses) {
        if (!session->emit(std::move(response))) {
            co_await send_error(exchange, 409, -32000, "MCP SSE stream is unavailable");
            co_return;
        }
    }
    co_await send_body(exchange, 200, "application/json", {});
}

async::Task<void> McpHttpHandler::stream(http::HttpExchange &exchange, const std::shared_ptr<McpSession> &session,
                                         const std::shared_ptr<McpStreamMailbox> &mailbox,
                                         std::string endpoint) noexcept {
    http::HttpHeaders headers(exchange.pool());
    headers.set_view("Content-Type", "text/event-stream");
    headers.set_view("Cache-Control", "no-cache, no-transform");
    headers.set_view("X-Accel-Buffering", "no");
    headers.set_view("Access-Control-Allow-Origin", "*");
    headers.set(kSessionHeader, session->id());
    auto sent = co_await exchange.send_header({
            .kind = http::OutgoingHeaderKind::Final,
            .status_code = 200,
            .headers = &headers,
            .body = http::HttpBodySpec::Stream(),
            .connection_mode = http::ResponseConnectionMode::Auto,
            .end_stream = false,
    });
    if (!sent) {
        session->detach_stream(mailbox, event::EventLoop::current().now());
        co_return;
    }
    if (!endpoint.empty()) {
        std::string event = "event: endpoint\ndata: ";
        event.append(endpoint);
        event.append("\n\n");
        auto written =
                co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(event.data()), event.size(), false);
        if (!written) {
            session->detach_stream(mailbox, event::EventLoop::current().now());
            co_return;
        }
    }

    auto next_heartbeat = event::EventLoop::current().now() + kHeartbeatInitialDelay;
    std::uint64_t heartbeat_id = 0;
    auto changes = mailbox->subscribe();
    auto change = changes.current();
    while (!mailbox->closed() && !exchange.response_channel_closed()) {
        std::vector<std::string> messages = mailbox->take();
        const auto now = event::EventLoop::current().now();
        if (now >= next_heartbeat) {
            messages.push_back(McpProtocol::ping_request(++heartbeat_id));
            next_heartbeat = now + kHeartbeatInterval;
        }
        for (const std::string &message: messages) {
            std::string event = "event: message\ndata: ";
            event.append(message);
            event.append("\n\n");
            auto written = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(event.data()),
                                                       event.size(), false);
            if (!written) {
                mailbox->close();
                break;
            }
        }
        if (!mailbox->closed() && !exchange.response_channel_closed()) {
            const auto current = event::EventLoop::current().now();
            const auto delay = current < next_heartbeat
                                       ? std::chrono::ceil<std::chrono::milliseconds>(next_heartbeat - current)
                                       : std::chrono::milliseconds::zero();
            auto wake =
                    co_await async::when_any([delay]() { return async::sleep(delay); },
                                             [&changes, version = change.version]() { return changes.next(version); },
                                             [&exchange]() { return exchange.wait_response_channel_closed(); });
            if (wake.is<1>()) {
                change = std::move(wake).get<1>();
            } else if (wake.is<2>()) {
                std::move(wake).get<2>();
                break;
            } else {
                std::move(wake).get<0>();
            }
        }
    }
    if (!exchange.response_channel_closed()) {
        (void) co_await exchange.write_all(nullptr, 0, true);
    }
    session->detach_stream(mailbox, event::EventLoop::current().now());
}

} // namespace fiber::ai_server
