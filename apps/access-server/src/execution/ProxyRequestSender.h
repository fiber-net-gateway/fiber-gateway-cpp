#ifndef FIBER_ACCESS_SERVER_PROXY_REQUEST_SENDER_H
#define FIBER_ACCESS_SERVER_PROXY_REQUEST_SENDER_H

#include "../../../../src/async/Task.h"
#include "../../../../src/common/IoError.h"
#include "../../../../src/http/ClientHttp1Exchange.h"
#include "../../../../src/http/LocalHttp1ConnectionPoolSet.h"
#include "../../../../src/net/IpAddress.h"
#include "../observability/AccessRequestTelemetry.h"
#include "ProxyRequestPlan.h"

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
    ~ProxyUpstreamResponse();

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
                          std::unique_ptr<http::ClientHttp1Exchange> exchange, const http::Http1ResponseHead *head,
                          AccessProviderTransaction provider_transaction) noexcept;
    void finish_provider_transaction() noexcept;

    ProxyUpstreamEndpoint endpoint_;
    // exchange_ must be destroyed before lease_ so the connection becomes idle
    // before the lease returns it to the pool.
    http::LocalHttp1ConnectionPoolSet::Lease lease_;
    std::unique_ptr<http::ClientHttp1Exchange> exchange_;
    const http::Http1ResponseHead *head_ = nullptr;
    AccessProviderTransaction provider_transaction_;
};

using ProxyUpstreamResponseResult = std::expected<ProxyUpstreamResponse, ProxyRequestError>;

class ProxyRequestSender {
public:
    ProxyRequestSender(http::LocalHttp1ConnectionPoolSet &pool, ProxyClusterMatcher cluster_matcher = {},
                       ProxyDnsResolver dns_resolver = {}, ProxyRequestSenderOptions options = {}) noexcept;

    [[nodiscard]] async::Task<ProxyUpstreamResponseResult> start(http::HttpExchange &downstream,
                                                                 const PreparedProxyRequest &request,
                                                                 AccessRequestTelemetry *telemetry = nullptr) noexcept;

private:
    struct ConnectedEndpoint {
        http::LocalHttp1ConnectionPoolSet::Lease lease;
        http::Http1ClientConnection *connection = nullptr;
    };

    [[nodiscard]] async::Task<std::expected<ConnectedEndpoint, ProxyRequestError>>
    connect(const ProxyUpstreamEndpoint &endpoint) noexcept;

    http::LocalHttp1ConnectionPoolSet *pool_ = nullptr;
    ProxyClusterMatcher cluster_matcher_{};
    ProxyDnsResolver dns_resolver_{};
    ProxyRequestSenderOptions options_{};
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_PROXY_REQUEST_SENDER_H
