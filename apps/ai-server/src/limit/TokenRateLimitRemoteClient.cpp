#include "TokenRateLimitRemoteClient.h"

#include "../observability/AiServerCatRequest.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>

#include <fiber/common/Assert.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/event/EventLoop.h>
#include <fiber/http/ClientHttp1Exchange.h>
#include <fiber/http/ClientHttp1Types.h>
#include <fiber/http/Http1ConnectionGroupKey.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>

namespace fiber::ai_server {
namespace {

using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kRequestTimeout{3000};
constexpr std::string_view kCheckPath = "/internal/llm/rate-limit/check";
constexpr std::string_view kSettlePath = "/internal/llm/rate-limit/settle";

RateLimitRemoteError error(RateLimitRemoteErrorCode code, common::IoErr io_error = common::IoErr::None,
                           int status_code = 0) noexcept {
    return RateLimitRemoteError{
            .code = code,
            .io_error = io_error,
            .status_code = status_code,
    };
}

std::chrono::milliseconds remaining(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = event::EventLoop::current().now();
    if (now >= deadline) {
        return std::chrono::milliseconds::zero();
    }
    return std::max(1ms, std::chrono::ceil<std::chrono::milliseconds>(deadline - now));
}

std::string host_header(const RateLimitNode &node) {
    std::string output;
    output.reserve(node.host.size() + 8);
    output.append(node.host);
    output.push_back(':');
    output.append(std::to_string(node.port));
    return output;
}

} // namespace

TokenRateLimitRemoteClient::TokenRateLimitRemoteClient(event::EventLoopGroup &workers) noexcept :
    pool_(workers, http::Http1ConnectionPoolCore::Options{
                           .max_idle_per_group = 2,
                           .max_idle_total = 64,
                           .idle_timeout = std::chrono::seconds(30),
                           .initial_group_capacity = 8,
                   }) {}

TokenRateLimitRemoteClient::~TokenRateLimitRemoteClient() { FIBER_ASSERT(!initialized_); }

bool TokenRateLimitRemoteClient::init() noexcept {
    if (initialized_) {
        return true;
    }
    initialized_ = pool_.init();
    return initialized_;
}

async::Task<void> TokenRateLimitRemoteClient::shutdown() noexcept {
    if (!initialized_) {
        co_return;
    }
    co_await pool_.shutdown_async();
    initialized_ = false;
}

async::Task<std::expected<RateLimitCheckResponse, RateLimitRemoteError>>
TokenRateLimitRemoteClient::check(const RateLimitNode &node, const RateLimitCheckRequest &request,
                                  const cat::MessageTraceContext *cat_context, std::string_view trace_state) noexcept {
    auto encoded = encode_rate_limit_check_request(request);
    if (!encoded) {
        co_return std::unexpected(
                error(RateLimitRemoteErrorCode::InvalidNode, encoded.error().code == RateLimitPayloadErrorCode::TooLarge
                                                                     ? common::IoErr::MessageTooLarge
                                                                     : common::IoErr::Invalid));
    }
    auto response = co_await post(node, kCheckPath, std::move(*encoded), cat_context, trace_state);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    mem::BufPool pool;
    auto decoded = decode_rate_limit_check_response(*response, pool);
    if (!decoded) {
        co_return std::unexpected(error(RateLimitRemoteErrorCode::InvalidResponse,
                                        decoded.error().code == RateLimitPayloadErrorCode::TooLarge
                                                ? common::IoErr::MessageTooLarge
                                                : common::IoErr::Invalid));
    }
    co_return *decoded;
}

async::Task<std::expected<RateLimitSettleResponse, RateLimitRemoteError>>
TokenRateLimitRemoteClient::settle(const RateLimitNode &node, const RateLimitSettleRequest &request,
                                   const cat::MessageTraceContext *cat_context, std::string_view trace_state) noexcept {
    auto encoded = encode_rate_limit_settle_request(request);
    if (!encoded) {
        co_return std::unexpected(
                error(RateLimitRemoteErrorCode::InvalidNode, encoded.error().code == RateLimitPayloadErrorCode::TooLarge
                                                                     ? common::IoErr::MessageTooLarge
                                                                     : common::IoErr::Invalid));
    }
    auto response = co_await post(node, kSettlePath, std::move(*encoded), cat_context, trace_state);
    if (!response) {
        co_return std::unexpected(response.error());
    }
    mem::BufPool pool;
    auto decoded = decode_rate_limit_settle_response(*response, pool);
    if (!decoded) {
        co_return std::unexpected(error(RateLimitRemoteErrorCode::InvalidResponse,
                                        decoded.error().code == RateLimitPayloadErrorCode::TooLarge
                                                ? common::IoErr::MessageTooLarge
                                                : common::IoErr::Invalid));
    }
    co_return *decoded;
}

async::Task<std::expected<std::string, RateLimitRemoteError>>
TokenRateLimitRemoteClient::post(const RateLimitNode &node, std::string_view path, std::string request_body,
                                 const cat::MessageTraceContext *cat_context, std::string_view trace_state) noexcept {
    if (!initialized_) {
        co_return std::unexpected(error(RateLimitRemoteErrorCode::PoolUnavailable, common::IoErr::Canceled));
    }
    net::IpAddress ip;
    if (node.port == 0 || !net::IpAddress::parse(node.host, ip) || !ip.is_v4() || ip.is_unspecified()) {
        co_return std::unexpected(error(RateLimitRemoteErrorCode::InvalidNode, common::IoErr::Invalid));
    }
    const auto key = http::Http1ConnectionGroupKey::from_ip(ip, node.port, http::Http1ConnectionGroupKey::Scheme::Http);
    auto lease = pool_.acquire(key);
    if (!lease.valid()) {
        co_return std::unexpected(error(RateLimitRemoteErrorCode::PoolUnavailable, common::IoErr::Canceled));
    }
    http::Http1ClientConnection *connection = lease.get();
    const auto deadline = event::EventLoop::current().now() + kRequestTimeout;
    if (!connection) {
        auto emplaced = lease.emplace_connection(http::Http1ClientConnectionOptions{
                .peer_addr = net::SocketAddress(ip, node.port),
        });
        if (!emplaced) {
            co_return std::unexpected(error(RateLimitRemoteErrorCode::Connect, emplaced.error()));
        }
        connection = *emplaced;
        auto connected = co_await connection->connect(remaining(deadline));
        if (!connected) {
            co_return std::unexpected(error(RateLimitRemoteErrorCode::Connect, connected.error()));
        }
    }

    mem::BufPool request_pool;
    http::ClientHttp1Exchange exchange(*connection, request_pool);
    http::HttpHeaders headers(request_pool);
    const std::string host = host_header(node);
    headers.set("Host", host);
    headers.set_view("Content-Type", "application/json");
    headers.set_view("Accept", "application/json");
    (void) inject_cat_headers(headers, cat_context, trace_state);
    http::Http1RequestHead head{
            .method = http::HttpMethod::Post,
            .target = path,
            .headers = &headers,
            .body = http::HttpBodySpec::ContentLength(request_body.size()),
    };
    auto sent_header = co_await exchange.send_header(head, request_body.empty(), remaining(deadline));
    if (!sent_header) {
        co_return std::unexpected(error(RateLimitRemoteErrorCode::Send, sent_header.error()));
    }
    if (!request_body.empty()) {
        auto sent_body = co_await exchange.write_all(reinterpret_cast<const std::uint8_t *>(request_body.data()),
                                                     request_body.size(), true, remaining(deadline));
        if (!sent_body || *sent_body != request_body.size()) {
            co_return std::unexpected(
                    error(RateLimitRemoteErrorCode::Send, sent_body ? common::IoErr::Invalid : sent_body.error()));
        }
    }

    const http::Http1ResponseHead *response = nullptr;
    for (;;) {
        auto received = co_await exchange.read_header(remaining(deadline));
        if (!received) {
            co_return std::unexpected(error(RateLimitRemoteErrorCode::Receive, received.error()));
        }
        if (!(*received)->is_informational()) {
            response = *received;
            break;
        }
    }
    if (!response || response->status_code < 200 || response->status_code >= 300) {
        co_return std::unexpected(error(RateLimitRemoteErrorCode::HttpStatus, common::IoErr::Invalid,
                                        response ? response->status_code : 0));
    }

    std::string response_body;
    response_body.reserve(256);
    for (;;) {
        const std::size_t remaining_bytes = kMaxRateLimitHttpBodyBytes - response_body.size();
        auto chunk =
                co_await exchange.read_body(std::min<std::size_t>(16 * 1024, remaining_bytes + 1), remaining(deadline));
        if (!chunk) {
            co_return std::unexpected(error(RateLimitRemoteErrorCode::Receive, chunk.error()));
        }
        if (chunk->readable_bytes() > remaining_bytes) {
            (void) exchange.abort(common::IoErr::MessageTooLarge);
            co_return std::unexpected(
                    error(RateLimitRemoteErrorCode::ResponseTooLarge, common::IoErr::MessageTooLarge));
        }
        while (const mem::IoBuf *part = chunk->first_readable()) {
            const std::size_t size = part->readable();
            response_body.append(reinterpret_cast<const char *>(part->readable_data()), size);
            chunk->consume_and_compact(size);
        }
        if (chunk->complete()) {
            break;
        }
    }
    if (response_body.empty()) {
        co_return std::unexpected(error(RateLimitRemoteErrorCode::InvalidResponse, common::IoErr::Invalid));
    }
    co_return response_body;
}

} // namespace fiber::ai_server
