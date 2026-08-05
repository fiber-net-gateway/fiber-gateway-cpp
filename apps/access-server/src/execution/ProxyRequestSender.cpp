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
#include <array>
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

http::Http1ClientConnectionOptions connection_options(const http::Http1ConnectionGroupKey &key,
                                                      const net::IpAddress &ip) {
    http::Http1ClientConnectionOptions result;
    result.peer_addr = net::SocketAddress(ip, key.port());
    if (key.scheme() == http::Http1ConnectionGroupKey::Scheme::Https) {
        result.tls.enabled = true;
        // The Java client uses InsecureTrustManagerFactory by default.
        result.tls.verify_peer = false;
        if (key.is_name()) {
            result.tls.server_name.assign(key.host_name());
        }
    }
    return result;
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
                                             const http::Http1ResponseHead *head,
                                             AccessProviderTransaction provider_transaction) noexcept :
    endpoint_(std::move(endpoint)), lease_(std::move(lease)), exchange_(std::move(exchange)), head_(head),
    provider_transaction_(std::move(provider_transaction)) {}

ProxyUpstreamResponse::~ProxyUpstreamResponse() { finish_provider_transaction(); }

ProxyUpstreamResponse &ProxyUpstreamResponse::operator=(ProxyUpstreamResponse &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    finish_provider_transaction();
    exchange_.reset();
    lease_.reset();
    endpoint_ = std::move(other.endpoint_);
    lease_ = std::move(other.lease_);
    exchange_ = std::move(other.exchange_);
    head_ = other.head_;
    provider_transaction_ = std::move(other.provider_transaction_);
    other.head_ = nullptr;
    return *this;
}

void ProxyUpstreamResponse::finish_provider_transaction() noexcept {
    if (!provider_transaction_.valid()) {
        return;
    }
    if (exchange_ && exchange_->response_complete() && head_) {
        provider_transaction_.complete(head_->status_code);
        return;
    }
    provider_transaction_.fail("canceled", common::IoErr::Canceled);
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
    auto result = co_await exchange_->read_body(max_bytes, timeout);
    if (!result) {
        provider_transaction_.fail("read_response_body", result.error());
        co_return std::unexpected(result.error());
    }
    if (result->complete() && head_) {
        provider_transaction_.complete(head_->status_code);
    }
    co_return std::move(*result);
}

async::Task<common::IoResult<void>> ProxyUpstreamResponse::discard_body(std::chrono::milliseconds timeout) noexcept {
    if (!exchange_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    auto result = co_await exchange_->discard_response_body(timeout);
    if (!result) {
        provider_transaction_.fail("read_response_body", result.error());
        co_return std::unexpected(result.error());
    }
    if (head_) {
        provider_transaction_.complete(head_->status_code);
    }
    co_return common::IoResult<void>{};
}

common::IoResult<void> ProxyUpstreamResponse::switch_to_raw_stream() noexcept {
    if (!exchange_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto result = exchange_->switch_to_raw_stream();
    if (!result) {
        provider_transaction_.fail("switch_to_raw_stream", result.error());
    }
    return result;
}

async::Task<void> ProxyUpstreamResponse::relay_websocket(http::HttpExchange &downstream,
                                                         std::chrono::milliseconds timeout) noexcept {
    if (!exchange_) {
        co_return;
    }
    co_await http::proxy_core::relay_websocket_tunnel(downstream, *exchange_, timeout, timeout);
    if (head_) {
        provider_transaction_.complete(head_->status_code);
    }
}

common::IoResult<void> ProxyUpstreamResponse::abort(common::IoErr reason) noexcept {
    if (!exchange_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto result = exchange_->abort(reason);
    provider_transaction_.fail("aborted", reason == common::IoErr::None ? common::IoErr::Canceled : reason);
    return result;
}

ProxyRequestSender::ProxyRequestSender(http::LocalHttp1ConnectionPoolSet &pool, ProxyClusterMatcher cluster_matcher,
                                       ProxyDnsResolver dns_resolver, ProxyRequestSenderOptions options) noexcept :
    pool_(&pool), cluster_matcher_(cluster_matcher), dns_resolver_(dns_resolver), options_(options) {
    if (options_.body_chunk_size == 0) {
        options_.body_chunk_size = 64 * 1024;
    }
}

async::Task<std::expected<ProxyRequestSender::ConnectedEndpoint, ProxyRequestError>>
ProxyRequestSender::connect(const ProxyUpstreamEndpoint &endpoint) noexcept {
    if (endpoint.connection_key == nullptr) {
        co_return std::unexpected(error(ProxyRequestErrorCode::Connect,
                                        "upstream cannot be used as a connection pool key", common::IoErr::Invalid));
    }
    const http::Http1ConnectionGroupKey &key = *endpoint.connection_key;

    ConnectedEndpoint output;
    output.lease = pool_->acquire(key);
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
    if (key.is_ip()) {
        addresses = std::span(&key.ip_address(), 1);
    } else {
        if (!dns_resolver_.resolve) {
            co_return std::unexpected(error(ProxyRequestErrorCode::ResolveUpstream,
                                            "upstream DNS resolver is unavailable", common::IoErr::NotFound));
        }
        auto result = co_await dns_resolver_.resolve(dns_resolver_.context, key.host_name());
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
            output.lease = pool_->acquire(key);
            if (!output.lease.valid()) {
                co_return std::unexpected(error(ProxyRequestErrorCode::PoolShutdown,
                                                "upstream connection pool is shutting down", common::IoErr::Canceled));
            }
            if (output.lease.has_connection()) {
                output.connection = output.lease.get();
                co_return std::move(output);
            }
        }

        auto emplaced = output.lease.emplace_connection(connection_options(key, addresses[i]));
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

async::Task<ProxyUpstreamResponseResult> ProxyRequestSender::start(http::HttpExchange &downstream,
                                                                   const PreparedProxyRequest &request,
                                                                   AccessRequestTelemetry *telemetry) noexcept {
    if (!pool_) {
        co_return std::unexpected(error(ProxyRequestErrorCode::PoolShutdown, "upstream connection pool is unavailable",
                                        common::IoErr::Invalid));
    }
    if (request.address_selector == nullptr) {
        co_return std::unexpected(error(ProxyRequestErrorCode::SelectUpstream,
                                        "upstream address selector is unavailable", common::IoErr::NotFound));
    }

    std::optional<std::string_view> cluster_override;
    if (request.context_cluster) {
        cluster_override = *request.context_cluster;
    }
    if (cluster_matcher_.matches && cluster_matcher_.matches(cluster_matcher_.context, downstream)) {
        cluster_override = std::string_view("gray");
    }

    std::array<std::uint64_t, kMaxJavaAttempts> excluded_selection_tokens{};
    std::size_t excluded_selection_token_count = 0;
    std::optional<ProxyRequestError> previous_error;
    const auto report_selection = [&](ProxyUpstreamEndpoint &endpoint, bool success) noexcept {
        request.address_selector->report_address(endpoint, success);
    };

    for (std::size_t attempt = 0; attempt < kMaxJavaAttempts; ++attempt) {
        auto selected = request.address_selector->select_address(
                cluster_override,
                std::span<const std::uint64_t>(excluded_selection_tokens.data(), excluded_selection_token_count));
        if (!selected) {
            co_return std::unexpected(previous_error ? *previous_error
                                                     : error(ProxyRequestErrorCode::SelectUpstream,
                                                             selected.error().message, selected.error().io_error));
        }
        if (selected->selection_token == 0) {
            co_return std::unexpected(error(ProxyRequestErrorCode::SelectUpstream,
                                            "upstream selector returned an invalid selection token",
                                            common::IoErr::Invalid));
        }

        const std::string_view provider_name =
                selected->provider_name.empty() ? selected->host_header : selected->provider_name;
        AccessProviderTransaction provider_transaction =
                telemetry ? telemetry->start_provider_transaction(provider_name) : AccessProviderTransaction{};
        provider_transaction.add_upstream(selected->host_header, attempt + 1);

        auto connected = co_await connect(*selected);
        if (!connected) {
            provider_transaction.fail(proxy_request_error_code_name(connected.error().code),
                                      connected.error().io_error);
            report_selection(*selected, false);
            FIBER_ASSERT(excluded_selection_token_count < excluded_selection_tokens.size());
            excluded_selection_tokens[excluded_selection_token_count++] = selected->selection_token;
            previous_error = connected.error();
            continue;
        }
        provider_transaction.add_connection_reuse(connected->connection->request_count());
        if (telemetry) {
            telemetry->set_upstream(*selected);
        }

        http::HttpHeaders headers(downstream.pool());
        if (!build_request_headers(*selected, request, headers) ||
            (telemetry && !telemetry->inject_upstream_headers(headers, provider_transaction))) {
            provider_transaction.fail("build_headers", common::IoErr::NoMem);
            co_return std::unexpected(error(ProxyRequestErrorCode::BuildHeaders,
                                            "failed to build upstream request headers", common::IoErr::NoMem));
        }

        auto upstream = std::unique_ptr<http::ClientHttp1Exchange>(
                new (std::nothrow) http::ClientHttp1Exchange(*connected->connection, downstream.pool()));
        if (!upstream || !upstream->valid()) {
            provider_transaction.fail("create_exchange", upstream ? common::IoErr::Busy : common::IoErr::NoMem);
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
            provider_transaction.fail("send_header", sent_header.error());
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
                    provider_transaction.fail("read_request_body", body.error());
                    report_selection(*selected, false);
                    co_return std::unexpected(error(ProxyRequestErrorCode::ReadRequestBody,
                                                    "failed to read downstream request body", body.error()));
                }
                const bool complete = body->complete();
                const std::size_t body_bytes = body->readable_bytes();
                if (!forward_state.accepts(body_bytes)) {
                    (void) upstream->abort(common::IoErr::Invalid);
                    provider_transaction.fail("read_request_body", common::IoErr::Invalid);
                    report_selection(*selected, false);
                    co_return std::unexpected(error(ProxyRequestErrorCode::ReadRequestBody,
                                                    "downstream request body does not match Content-Length",
                                                    common::IoErr::Invalid));
                }
                if (request.max_request_body_size != 0 &&
                    body_bytes > request.max_request_body_size - std::min(received, request.max_request_body_size)) {
                    (void) upstream->abort(common::IoErr::MessageTooLarge);
                    provider_transaction.fail("request_body_too_large", common::IoErr::MessageTooLarge);
                    report_selection(*selected, false);
                    co_return std::unexpected(error(ProxyRequestErrorCode::RequestBodyTooLarge,
                                                    "downstream request body exceeds route limit",
                                                    common::IoErr::MessageTooLarge));
                }
                received += body_bytes;
                if (forward_state.should_write(body_bytes)) {
                    auto written = co_await upstream->write_all(std::move(*body));
                    if (!written) {
                        provider_transaction.fail("send_request_body", written.error());
                        report_selection(*selected, false);
                        co_return std::unexpected(error(ProxyRequestErrorCode::SendRequestBody,
                                                        "failed to send upstream request body", written.error()));
                    }
                    if (*written != body_bytes) {
                        (void) upstream->abort(common::IoErr::Invalid);
                        provider_transaction.fail("send_request_body", common::IoErr::Invalid);
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
                        provider_transaction.fail("read_request_body", common::IoErr::Invalid);
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
                provider_transaction.fail("read_response_header", received.error());
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
                                        response_head, std::move(provider_transaction));
    }

    co_return std::unexpected(previous_error.value_or(
            error(ProxyRequestErrorCode::SelectUpstream, "no usable upstream", common::IoErr::NotFound)));
}

} // namespace fiber::access_server
