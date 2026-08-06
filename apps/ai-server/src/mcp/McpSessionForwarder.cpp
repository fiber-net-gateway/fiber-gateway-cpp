#include "McpSessionForwarder.h"

#include <algorithm>
#include <chrono>
#include <string>

#include <common/Assert.h>
#include <event/EventLoop.h>
#include <http/ClientHttp1Exchange.h>
#include <http/ClientHttp1Types.h>
#include <http/Http1ConnectionGroupKey.h>
#include <http/HttpBodyPipe.h>
#include <http/HttpBodySpec.h>
#include <http/HttpExchange.h>
#include <http/HttpHeaders.h>
#include <http/HttpProxyCore.h>
#include <net/IpAddress.h>
#include <net/SocketAddress.h>

namespace fiber::ai_server {
namespace {

using namespace http::proxy_core;
using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kHeaderTimeout{5000};

std::string host_header(const RateLimitNode &node) {
    std::string output;
    output.reserve(node.host.size() + 8);
    output.append(node.host);
    output.push_back(':');
    output.append(std::to_string(node.port));
    return output;
}

} // namespace

McpSessionForwarder::McpSessionForwarder(event::EventLoopGroup &workers, RateLimitShardRing &ring) noexcept :
    ring_(&ring), pool_(workers, http::Http1ConnectionPoolCore::Options{
                                         .max_idle_per_group = 2,
                                         .max_idle_total = 64,
                                         .idle_timeout = std::chrono::seconds(30),
                                         .initial_group_capacity = 8,
                                 }) {}

McpSessionForwarder::~McpSessionForwarder() { FIBER_ASSERT(!initialized_); }

bool McpSessionForwarder::init() noexcept {
    if (!initialized_) {
        initialized_ = pool_.init();
    }
    return initialized_;
}

async::Task<void> McpSessionForwarder::shutdown() noexcept {
    if (initialized_) {
        co_await pool_.shutdown_async();
        initialized_ = false;
    }
}

async::Task<McpForwardResult> McpSessionForwarder::forward(http::HttpExchange &exchange,
                                                           std::string_view node_id) noexcept {
    if (!initialized_) {
        co_return McpForwardResult::Failed;
    }
    const auto node = ring_->find_node(node_id);
    net::IpAddress ip;
    if (!node || node->local || node->port == 0 || !net::IpAddress::parse(node->host, ip) || !ip.is_v4() ||
        ip.is_unspecified()) {
        co_return node ? McpForwardResult::Failed : McpForwardResult::NodeNotFound;
    }
    const auto key =
            http::Http1ConnectionGroupKey::from_ip(ip, node->port, http::Http1ConnectionGroupKey::Scheme::Http);
    auto lease = pool_.acquire(key);
    if (!lease.valid()) {
        co_return McpForwardResult::Failed;
    }
    http::Http1ClientConnection *connection = lease.get();
    if (!connection) {
        auto created = lease.emplace_connection(http::Http1ClientConnectionOptions{
                .peer_addr = net::SocketAddress(ip, node->port),
        });
        if (!created) {
            co_return McpForwardResult::Failed;
        }
        connection = *created;
        auto connected = co_await connection->connect(kHeaderTimeout);
        if (!connected) {
            co_return McpForwardResult::Failed;
        }
    }

    http::HttpHeaders request_headers(exchange.pool());
    for (const auto &field: exchange.request_headers()) {
        if (field.name_len == 0 || field.lowcase_view() == "host" || is_request_framing_header(field) ||
            should_skip_hop_by_hop_header(exchange.request_headers(), field)) {
            continue;
        }
        request_headers.add_view(field.name_view(), field.value_view(), field.lowcase_name, field.name_hash);
    }
    const std::string host = host_header(*node);
    request_headers.set("Host", host);
    bool request_end = true;
    const http::HttpBodySpec request_body = detect_request_body(exchange, request_end);
    std::string target_scratch;
    const std::string_view target = request_target_view(exchange.uri(), target_scratch);
    http::ClientHttp1Exchange upstream(*connection, exchange.pool());
    auto request_sent = co_await upstream.send_header(
            {
                    .method = exchange.method(),
                    .target = target,
                    .headers = &request_headers,
                    .body = request_body,
            },
            request_end, kHeaderTimeout);
    if (!request_sent) {
        co_return McpForwardResult::Failed;
    }
    if (!request_end) {
        auto piped = co_await http::pipe_http_body(http::make_http_body_pipe_reader(exchange),
                                                   http::make_http_body_pipe_writer(upstream),
                                                   event::EventLoop::current().io_buf_node_pool(),
                                                   http::HttpBodyPipeOptions{
                                                           .read_timeout = std::chrono::milliseconds::max(),
                                                           .write_timeout = kHeaderTimeout,
                                                   });
        if (!piped) {
            co_return McpForwardResult::Failed;
        }
    }

    const http::Http1ResponseHead *response = nullptr;
    for (;;) {
        auto received = co_await upstream.read_header(kHeaderTimeout);
        if (!received) {
            co_return McpForwardResult::Failed;
        }
        if (!(*received)->is_informational()) {
            response = *received;
            break;
        }
    }
    http::HttpHeaders response_headers(exchange.pool());
    for (const auto &field: response->headers) {
        if (field.name_len == 0 || should_skip_hop_by_hop_header(response->headers, field)) {
            continue;
        }
        response_headers.add_view(field.name_view(), field.value_view(), field.lowcase_name, field.name_hash);
    }
    const bool no_body = response_has_no_body(exchange.method(), response->status_code);
    std::size_t content_length = 0;
    const bool has_content_length =
            !no_body &&
            parse_decimal(response->headers.get("content-length", http::http_header_name_hash("content-length")),
                          content_length);
    const http::HttpBodySpec response_body =
            no_body ? http::HttpBodySpec::None()
                    : (has_content_length ? http::HttpBodySpec::ContentLength(content_length)
                                          : http::HttpBodySpec::Auto());
    auto header_sent = co_await exchange.send_header({
            .kind = http::OutgoingHeaderKind::Final,
            .status_code = response->status_code,
            .reason = response->reason,
            .headers = &response_headers,
            .body = response_body,
            .connection_mode = http::ResponseConnectionMode::Auto,
            .end_stream = no_body || (has_content_length && content_length == 0),
    });
    if (!header_sent) {
        co_return McpForwardResult::Failed;
    }
    if (no_body) {
        (void) co_await upstream.discard_response_body(kHeaderTimeout);
        co_return McpForwardResult::Completed;
    }
    if (has_content_length && content_length == 0) {
        co_return McpForwardResult::Completed;
    }
    auto response_piped = co_await http::pipe_http_body(http::make_http_body_pipe_reader(upstream),
                                                        http::make_http_body_pipe_writer(exchange),
                                                        event::EventLoop::current().io_buf_node_pool(),
                                                        http::HttpBodyPipeOptions{
                                                                .low_water = http::kUnbufferedBodyPipeLowWater,
                                                                .read_timeout = std::chrono::milliseconds::max(),
                                                                .write_timeout = std::chrono::milliseconds::max(),
                                                        });
    co_return response_piped ? McpForwardResult::Completed : McpForwardResult::Failed;
}

} // namespace fiber::ai_server
