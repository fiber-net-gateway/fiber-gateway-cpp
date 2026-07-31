#ifndef FIBER_ACCESS_SERVER_PROXY_REQUEST_SENDER_H
#define FIBER_ACCESS_SERVER_PROXY_REQUEST_SENDER_H

#include "../../../../src/async/Task.h"
#include "../../../../src/common/IoError.h"
#include "../../../../src/http/ClientHttp1Exchange.h"
#include "../../../../src/http/LocalHttp1ConnectionPoolSet.h"
#include "../../../../src/net/IpAddress.h"
#include "ProxyRequestPlan.h"

#include <fiber/nacos/discovery/ServiceLoadBalancer.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace fiber::access_server {

class AccessRequestTelemetry;

enum class ProxyRequestErrorCode : std::uint8_t {
    SelectUpstream,
    ResolveUpstream,
    PoolShutdown,
    Connect,
    BuildHeaders,
    SendHeader,
    ReadRequestBody,
    RequestBodyTooLarge,
    SendRequestBody,
    ReadResponseHeader,
};

struct ProxyRequestError {
    ProxyRequestErrorCode code = ProxyRequestErrorCode::Connect;
    common::IoErr io_error = common::IoErr::None;
    const char *message = nullptr;
};

[[nodiscard]] std::string_view proxy_request_error_code_name(ProxyRequestErrorCode code) noexcept;

// service_instance pins the discovery generation for as long as host and
// host_header are consumed by the request.
struct ProxyUpstreamEndpoint {
    ProxyUpstreamScheme scheme = ProxyUpstreamScheme::Http;
    std::string_view host;
    std::uint16_t port = 80;
    std::string_view host_header;
    std::optional<net::IpAddress> ip_address;
    nacos::LoadBalancer::Instance service_instance;
    std::uint64_t selection_token = 0;
};

struct ProxyServiceSelector {
    using SelectFunction = std::expected<ProxyUpstreamEndpoint, ProxyRequestError> (*)(
            void *context, http::HttpExchange &exchange, std::string_view service,
            std::optional<std::string_view> cluster, std::span<const std::uint64_t> excluded_selection_tokens) noexcept;
    using ReportFunction = void (*)(void *context, ProxyUpstreamEndpoint &endpoint, bool success) noexcept;

    void *context = nullptr;
    SelectFunction select = nullptr;
    ReportFunction report = nullptr;
};

struct ProxyDnsResolver {
    using Function = async::Task<common::IoResult<std::vector<net::IpAddress>>> (*)(void *context,
                                                                                    std::string_view host) noexcept;

    void *context = nullptr;
    Function resolve = nullptr;
};

struct ProxyRequestSenderOptions {
    std::chrono::milliseconds connect_timeout{3000};
    std::size_t body_chunk_size = 64 * 1024;
};

class ProxyUpstreamResponse {
public:
    ProxyUpstreamResponse() noexcept = default;
    ProxyUpstreamResponse(const ProxyUpstreamResponse &) = delete;
    ProxyUpstreamResponse &operator=(const ProxyUpstreamResponse &) = delete;
    ProxyUpstreamResponse(ProxyUpstreamResponse &&) noexcept = default;
    ProxyUpstreamResponse &operator=(ProxyUpstreamResponse &&other) noexcept;
    ~ProxyUpstreamResponse() = default;

    [[nodiscard]] bool valid() const noexcept { return exchange_ != nullptr && head_ != nullptr; }
    [[nodiscard]] const http::Http1ResponseHead &head() const noexcept;
    [[nodiscard]] int status_code() const noexcept;
    [[nodiscard]] const ProxyUpstreamEndpoint &endpoint() const noexcept { return endpoint_; }

    [[nodiscard]] async::Task<common::IoResult<mem::IoBufChain>>
    read_body(std::size_t max_bytes = 64 * 1024,
              std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    [[nodiscard]] async::Task<common::IoResult<void>>
    discard_body(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    common::IoResult<void> switch_to_raw_stream() noexcept;
    [[nodiscard]] async::Task<void> relay_websocket(http::HttpExchange &downstream,
                                                    std::chrono::milliseconds timeout) noexcept;
    common::IoResult<void> abort(common::IoErr reason = common::IoErr::Canceled) noexcept;

private:
    friend class ProxyRequestSender;

    ProxyUpstreamResponse(ProxyUpstreamEndpoint endpoint, http::LocalHttp1ConnectionPoolSet::Lease lease,
                          std::unique_ptr<http::ClientHttp1Exchange> exchange,
                          const http::Http1ResponseHead *head) noexcept;

    ProxyUpstreamEndpoint endpoint_;
    // exchange_ must be destroyed before lease_ so the connection becomes idle
    // before the lease returns it to the pool.
    http::LocalHttp1ConnectionPoolSet::Lease lease_;
    std::unique_ptr<http::ClientHttp1Exchange> exchange_;
    const http::Http1ResponseHead *head_ = nullptr;
};

using ProxyUpstreamResponseResult = std::expected<ProxyUpstreamResponse, ProxyRequestError>;

class ProxyRequestSender {
public:
    ProxyRequestSender(http::LocalHttp1ConnectionPoolSet &pool, ProxyServiceSelector service_selector = {},
                       ProxyDnsResolver dns_resolver = {}, ProxyRequestSenderOptions options = {}) noexcept;

    [[nodiscard]] async::Task<ProxyUpstreamResponseResult> start(http::HttpExchange &downstream,
                                                                 const PreparedProxyRequest &request,
                                                                 AccessRequestTelemetry *telemetry = nullptr) noexcept;

private:
    struct ConnectedEndpoint {
        http::LocalHttp1ConnectionPoolSet::Lease lease;
        http::Http1ClientConnection *connection = nullptr;
    };

    [[nodiscard]] ProxyUpstreamEndpoint select_static_endpoint(const PreparedProxyRequest &request,
                                                               std::size_t index) const noexcept;
    [[nodiscard]] async::Task<std::expected<ConnectedEndpoint, ProxyRequestError>>
    connect(const ProxyUpstreamEndpoint &endpoint) noexcept;
    void report(ProxyUpstreamEndpoint &endpoint, bool success) const noexcept;

    http::LocalHttp1ConnectionPoolSet *pool_ = nullptr;
    ProxyServiceSelector service_selector_{};
    ProxyDnsResolver dns_resolver_{};
    ProxyRequestSenderOptions options_{};
    std::atomic<std::size_t> next_static_address_{0};
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROXY_REQUEST_SENDER_H
