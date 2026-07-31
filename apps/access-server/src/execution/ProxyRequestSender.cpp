#include "ProxyRequestSender.h"
#include "../observability/AccessRequestTelemetry.h"

#include "../../../../src/common/Assert.h"
#include "../../../../src/event/EventLoop.h"
#include "../../../../src/http/Http1ClientConnection.h"
#include "../../../../src/http/Http1ConnectionGroupKey.h"
#include "../../../../src/http/HttpHeaderHash.h"
#include "../../../../src/http/HttpProxyCore.h"
#include "../../../../src/http/HttpWebSocketProxy.h"
#include "../../../../src/net/SocketAddress.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace fiber::access_server {
namespace {

constexpr std::size_t kMaxJavaAttempts = 4;

ProxyRequestError error(ProxyRequestErrorCode code, const char *message,
                        common::IoErr io_error = common::IoErr::None) noexcept {
    return ProxyRequestError{
            .code = code,
            .io_error = io_error,
            .message = message,
    };
}

http::Http1ConnectionGroupKey::Scheme connection_scheme(ProxyUpstreamScheme scheme) noexcept {
    return scheme == ProxyUpstreamScheme::Https ? http::Http1ConnectionGroupKey::Scheme::Https
                                                : http::Http1ConnectionGroupKey::Scheme::Http;
}

std::optional<http::Http1ConnectionGroupKey> connection_key(const ProxyUpstreamEndpoint &endpoint) noexcept {
    const auto scheme = connection_scheme(endpoint.scheme);
    if (endpoint.ip_address) {
        return http::Http1ConnectionGroupKey::from_ip(*endpoint.ip_address, endpoint.port, scheme);
    }
    return http::Http1ConnectionGroupKey::from_name(endpoint.host, endpoint.port, scheme);
}

http::Http1ClientConnectionOptions connection_options(const ProxyUpstreamEndpoint &endpoint, const net::IpAddress &ip) {
    http::Http1ClientConnectionOptions result;
    result.peer_addr = net::SocketAddress(ip, endpoint.port);
    if (endpoint.scheme == ProxyUpstreamScheme::Https) {
        result.tls.enabled = true;
        // The Java client uses InsecureTrustManagerFactory by default.
        result.tls.verify_peer = false;
        if (!endpoint.ip_address) {
            result.tls.server_name.assign(endpoint.host);
        }
    }
    return result;
}

bool same_endpoint(const ProxyUpstreamEndpoint &left, const ProxyUpstreamEndpoint &right) noexcept {
    if (left.selection_token != 0 && right.selection_token != 0) {
        return left.selection_token == right.selection_token;
    }
    return left.scheme == right.scheme && left.port == right.port && left.host == right.host;
}

bool is_header(std::string_view actual, std::string_view expected) noexcept {
    return http::http_header_name_equals_ci(actual, expected);
}

bool build_request_headers(const ProxyUpstreamEndpoint &endpoint, const PreparedProxyRequest &request,
                           http::HttpHeaders &headers) noexcept {
    if (!headers.set_view("Host", endpoint.host_header)) {
        return false;
    }
    for (const EvaluatedHeader &header: request.headers) {
        // ClientHttp1Exchange serializes framing from HttpBodySpec.
        if (is_header(header.name, "Content-Length") || is_header(header.name, "Transfer-Encoding")) {
            continue;
        }
        http::HttpHeaders::HeaderField *field = nullptr;
        if (is_header(header.name, "Host")) {
            field = headers.set_view(header.name, header.value);
        } else {
            field = headers.add_view(header.name, header.value);
        }
        if (!field) {
            return false;
        }
    }
    return true;
}

std::chrono::milliseconds response_header_timeout(std::int32_t timeout_millis) noexcept {
    if (timeout_millis < 0) {
        return std::chrono::milliseconds::max();
    }
    return std::chrono::milliseconds(timeout_millis);
}

} // namespace

std::string_view proxy_request_error_code_name(ProxyRequestErrorCode code) noexcept {
    switch (code) {
        case ProxyRequestErrorCode::SelectUpstream:
            return "select_upstream";
        case ProxyRequestErrorCode::ResolveUpstream:
            return "resolve_upstream";
        case ProxyRequestErrorCode::PoolShutdown:
            return "pool_shutdown";
        case ProxyRequestErrorCode::Connect:
            return "connect";
        case ProxyRequestErrorCode::BuildHeaders:
            return "build_headers";
        case ProxyRequestErrorCode::SendHeader:
            return "send_header";
        case ProxyRequestErrorCode::ReadRequestBody:
            return "read_request_body";
        case ProxyRequestErrorCode::RequestBodyTooLarge:
            return "request_body_too_large";
        case ProxyRequestErrorCode::SendRequestBody:
            return "send_request_body";
        case ProxyRequestErrorCode::ReadResponseHeader:
            return "read_response_header";
    }
    return "unknown";
}

ProxyUpstreamResponse::ProxyUpstreamResponse(ProxyUpstreamEndpoint endpoint,
                                             http::LocalHttp1ConnectionPoolSet::Lease lease,
                                             std::unique_ptr<http::ClientHttp1Exchange> exchange,
                                             const http::Http1ResponseHead *head) noexcept :
    endpoint_(std::move(endpoint)), lease_(std::move(lease)), exchange_(std::move(exchange)), head_(head) {}

ProxyUpstreamResponse &ProxyUpstreamResponse::operator=(ProxyUpstreamResponse &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    exchange_.reset();
    lease_.reset();
    endpoint_ = std::move(other.endpoint_);
    lease_ = std::move(other.lease_);
    exchange_ = std::move(other.exchange_);
    head_ = other.head_;
    other.head_ = nullptr;
    return *this;
}

const http::Http1ResponseHead &ProxyUpstreamResponse::head() const noexcept {
    FIBER_ASSERT(head_ != nullptr);
    return *head_;
}

int ProxyUpstreamResponse::status_code() const noexcept { return head().status_code; }

async::Task<common::IoResult<mem::IoBufChain>>
ProxyUpstreamResponse::read_body(std::size_t max_bytes, std::chrono::milliseconds timeout) noexcept {
    if (!exchange_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await exchange_->read_body(max_bytes, timeout);
}

async::Task<common::IoResult<void>> ProxyUpstreamResponse::discard_body(std::chrono::milliseconds timeout) noexcept {
    if (!exchange_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await exchange_->discard_response_body(timeout);
}

common::IoResult<void> ProxyUpstreamResponse::switch_to_raw_stream() noexcept {
    if (!exchange_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return exchange_->switch_to_raw_stream();
}

async::Task<void> ProxyUpstreamResponse::relay_websocket(http::HttpExchange &downstream,
                                                         std::chrono::milliseconds timeout) noexcept {
    if (!exchange_) {
        co_return;
    }
    co_await http::proxy_core::relay_websocket_tunnel(downstream, *exchange_, timeout, timeout);
}

common::IoResult<void> ProxyUpstreamResponse::abort(common::IoErr reason) noexcept {
    if (!exchange_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return exchange_->abort(reason);
}

ProxyRequestSender::ProxyRequestSender(http::LocalHttp1ConnectionPoolSet &pool, ProxyServiceSelector service_selector,
                                       ProxyDnsResolver dns_resolver, ProxyRequestSenderOptions options) noexcept :
    pool_(&pool), service_selector_(service_selector), dns_resolver_(dns_resolver), options_(options) {
    if (options_.body_chunk_size == 0) {
        options_.body_chunk_size = 64 * 1024;
    }
}

ProxyUpstreamEndpoint ProxyRequestSender::select_static_endpoint(const PreparedProxyRequest &request,
                                                                 std::size_t index) const noexcept {
    const CompiledProxyAddress &address = request.addresses[index % request.addresses.size()];
    return ProxyUpstreamEndpoint{
            .scheme = address.scheme,
            .host = address.host,
            .port = address.port,
            .host_header = address.host_header,
            .ip_address = address.ip_address,
    };
}

async::Task<std::expected<ProxyRequestSender::ConnectedEndpoint, ProxyRequestError>>
ProxyRequestSender::connect(const ProxyUpstreamEndpoint &endpoint) noexcept {
    const auto key = connection_key(endpoint);
    if (!key) {
        co_return std::unexpected(error(ProxyRequestErrorCode::Connect,
                                        "upstream cannot be used as a connection pool key", common::IoErr::Invalid));
    }

    ConnectedEndpoint output;
    output.lease = pool_->acquire(*key);
    if (!output.lease.valid()) {
        co_return std::unexpected(error(ProxyRequestErrorCode::PoolShutdown,
                                        "upstream connection pool is shutting down", common::IoErr::Canceled));
    }
    if (output.lease.has_connection()) {
        output.connection = output.lease.get();
        co_return std::move(output);
    }

    std::vector<net::IpAddress> resolved;
    std::span<const net::IpAddress> addresses;
    if (endpoint.ip_address) {
        addresses = std::span(&*endpoint.ip_address, 1);
    } else {
        if (!dns_resolver_.resolve) {
            co_return std::unexpected(error(ProxyRequestErrorCode::ResolveUpstream,
                                            "upstream DNS resolver is unavailable", common::IoErr::NotFound));
        }
        auto result = co_await dns_resolver_.resolve(dns_resolver_.context, endpoint.host);
        if (!result) {
            co_return std::unexpected(
                    error(ProxyRequestErrorCode::ResolveUpstream, "upstream DNS resolution failed", result.error()));
        }
        resolved = std::move(*result);
        addresses = resolved;
    }
    if (addresses.empty()) {
        co_return std::unexpected(error(ProxyRequestErrorCode::ResolveUpstream, "upstream DNS returned no address",
                                        common::IoErr::NotFound));
    }

    common::IoErr last_error = common::IoErr::NotFound;
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        if (i > 0) {
            output.lease = pool_->acquire(*key);
            if (!output.lease.valid()) {
                co_return std::unexpected(error(ProxyRequestErrorCode::PoolShutdown,
                                                "upstream connection pool is shutting down", common::IoErr::Canceled));
            }
            if (output.lease.has_connection()) {
                output.connection = output.lease.get();
                co_return std::move(output);
            }
        }

        auto emplaced = output.lease.emplace_connection(connection_options(endpoint, addresses[i]));
        if (!emplaced) {
            last_error = emplaced.error();
            output.lease.reset();
            continue;
        }
        auto connected = co_await (*emplaced)->connect(options_.connect_timeout);
        if (connected) {
            output.connection = *emplaced;
            co_return std::move(output);
        }
        last_error = connected.error();
        output.lease.reset();
    }
    co_return std::unexpected(error(ProxyRequestErrorCode::Connect, "upstream connection failed", last_error));
}

void ProxyRequestSender::report(const ProxyUpstreamEndpoint &endpoint, bool success) const noexcept {
    if (service_selector_.report) {
        service_selector_.report(service_selector_.context, endpoint, success);
    }
}

async::Task<ProxyUpstreamResponseResult> ProxyRequestSender::start(http::HttpExchange &downstream,
                                                                   const PreparedProxyRequest &request,
                                                                   AccessRequestTelemetry *telemetry) noexcept {
    if (!pool_) {
        co_return std::unexpected(error(ProxyRequestErrorCode::PoolShutdown, "upstream connection pool is unavailable",
                                        common::IoErr::Invalid));
    }
    if (request.upstream_kind == ProxyUpstreamKind::Addresses && request.addresses.empty()) {
        co_return std::unexpected(
                error(ProxyRequestErrorCode::SelectUpstream, "no static upstream address", common::IoErr::NotFound));
    }
    if (request.upstream_kind == ProxyUpstreamKind::Service && !service_selector_.select) {
        co_return std::unexpected(error(ProxyRequestErrorCode::SelectUpstream,
                                        "service upstream selector is unavailable", common::IoErr::NotFound));
    }

    const std::size_t static_start =
            request.upstream_kind == ProxyUpstreamKind::Addresses
                    ? next_static_address_.fetch_add(1, std::memory_order_relaxed) % request.addresses.size()
                    : 0;
    const std::size_t attempts = request.upstream_kind == ProxyUpstreamKind::Addresses
                                         ? std::min(kMaxJavaAttempts, request.addresses.size())
                                         : kMaxJavaAttempts;
    std::optional<ProxyUpstreamEndpoint> previous_endpoint;
    std::optional<ProxyRequestError> previous_error;
    const auto report_selection = [&](const ProxyUpstreamEndpoint &endpoint, bool success) noexcept {
        if (request.upstream_kind == ProxyUpstreamKind::Service) {
            report(endpoint, success);
        }
    };

    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
        std::expected<ProxyUpstreamEndpoint, ProxyRequestError> selected =
                request.upstream_kind == ProxyUpstreamKind::Addresses
                        ? std::expected<ProxyUpstreamEndpoint, ProxyRequestError>(
                                  select_static_endpoint(request, static_start + attempt))
                        : service_selector_.select(service_selector_.context, downstream, request.service,
                                                   request.context_cluster
                                                           ? std::optional<std::string_view>(*request.context_cluster)
                                                           : request.cluster);
        if (!selected) {
            co_return std::unexpected(previous_error ? *previous_error : selected.error());
        }
        if (previous_endpoint && same_endpoint(*previous_endpoint, *selected)) {
            co_return std::unexpected(previous_error ? *previous_error
                                                     : error(ProxyRequestErrorCode::SelectUpstream,
                                                             "service selector returned the failed upstream",
                                                             common::IoErr::NotFound));
        }

        auto connected = co_await connect(*selected);
        if (!connected) {
            report_selection(*selected, false);
            previous_endpoint = std::move(*selected);
            previous_error = connected.error();
            continue;
        }
        if (telemetry) {
            telemetry->set_upstream(*selected);
        }

        http::HttpHeaders headers(downstream.pool());
        if (!build_request_headers(*selected, request, headers) ||
            (telemetry && !telemetry->inject_upstream_headers(headers))) {
            co_return std::unexpected(error(ProxyRequestErrorCode::BuildHeaders,
                                            "failed to build upstream request headers", common::IoErr::NoMem));
        }

        auto upstream = std::unique_ptr<http::ClientHttp1Exchange>(
                new (std::nothrow) http::ClientHttp1Exchange(*connected->connection, downstream.pool()));
        if (!upstream || !upstream->valid()) {
            report_selection(*selected, false);
            co_return std::unexpected(error(ProxyRequestErrorCode::Connect, "failed to create upstream HTTP exchange",
                                            upstream ? common::IoErr::Busy : common::IoErr::NoMem));
        }

        const bool end_stream =
                request.body.is_none() || (request.body.is_content_length() && request.body.content_length() == 0);
        const http::Http1RequestHead head{
                .method = request.method,
                .target = request.request_target,
                .headers = &headers,
                .body = request.body,
        };
        auto sent_header = co_await upstream->send_header(head, end_stream);
        if (!sent_header) {
            report_selection(*selected, false);
            co_return std::unexpected(error(ProxyRequestErrorCode::SendHeader, "failed to send upstream request header",
                                            sent_header.error()));
        }

        if (!end_stream) {
            http::proxy_core::RequestBodyForwardState forward_state(request.body);
            std::size_t received = 0;
            for (;;) {
                auto body = co_await downstream.read_body(options_.body_chunk_size);
                if (!body) {
                    (void) upstream->abort(body.error());
                    report_selection(*selected, false);
                    co_return std::unexpected(error(ProxyRequestErrorCode::ReadRequestBody,
                                                    "failed to read downstream request body", body.error()));
                }
                const bool complete = body->complete();
                const std::size_t body_bytes = body->readable_bytes();
                if (!forward_state.accepts(body_bytes)) {
                    (void) upstream->abort(common::IoErr::Invalid);
                    report_selection(*selected, false);
                    co_return std::unexpected(error(ProxyRequestErrorCode::ReadRequestBody,
                                                    "downstream request body does not match Content-Length",
                                                    common::IoErr::Invalid));
                }
                if (request.max_request_body_size != 0 &&
                    body_bytes > request.max_request_body_size - std::min(received, request.max_request_body_size)) {
                    (void) upstream->abort(common::IoErr::MessageTooLarge);
                    report_selection(*selected, false);
                    co_return std::unexpected(error(ProxyRequestErrorCode::RequestBodyTooLarge,
                                                    "downstream request body exceeds route limit",
                                                    common::IoErr::MessageTooLarge));
                }
                received += body_bytes;
                if (forward_state.should_write(body_bytes)) {
                    auto written = co_await upstream->write_all(std::move(*body));
                    if (!written) {
                        report_selection(*selected, false);
                        co_return std::unexpected(error(ProxyRequestErrorCode::SendRequestBody,
                                                        "failed to send upstream request body", written.error()));
                    }
                    if (*written != body_bytes) {
                        (void) upstream->abort(common::IoErr::Invalid);
                        report_selection(*selected, false);
                        co_return std::unexpected(error(ProxyRequestErrorCode::SendRequestBody,
                                                        "upstream request body write was incomplete",
                                                        common::IoErr::Invalid));
                    }
                    forward_state.record_write(*written);
                }
                if (complete) {
                    if (request.body.is_content_length() && !forward_state.complete()) {
                        (void) upstream->abort(common::IoErr::Invalid);
                        report_selection(*selected, false);
                        co_return std::unexpected(error(ProxyRequestErrorCode::ReadRequestBody,
                                                        "downstream request body ended before Content-Length",
                                                        common::IoErr::Invalid));
                    }
                    break;
                }
            }
        }

        const http::Http1ResponseHead *response_head = nullptr;
        for (;;) {
            auto received = co_await upstream->read_header(response_header_timeout(request.timeout_millis));
            if (!received) {
                report_selection(*selected, false);
                co_return std::unexpected(error(ProxyRequestErrorCode::ReadResponseHeader,
                                                "failed to read upstream response header", received.error()));
            }
            if ((*received)->status_code == 101 || !(*received)->is_informational()) {
                response_head = *received;
                break;
            }
        }

        report_selection(*selected, response_head->status_code < 500);
        co_return ProxyUpstreamResponse(std::move(*selected), std::move(connected->lease), std::move(upstream),
                                        response_head);
    }

    co_return std::unexpected(previous_error.value_or(
            error(ProxyRequestErrorCode::SelectUpstream, "no usable upstream", common::IoErr::NotFound)));
}

} // namespace fiber::access_server
