#include "HttpScriptServices.h"

#include <utility>

#include <fiber/http/Http1ClientConnection.h>
#include <fiber/http_script/HttpScriptServices.h>
#include <fiber/http_script/HttpTarget.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TlsOptions.h>

#include "../upstream/ConnectionPool.h"
#include "../upstream/UpstreamConnection.h"
#include "../upstream/UpstreamRegistry.h"

namespace fiber::lite_nginx::runtime {
namespace {

// Wraps the unified AcquiredUpstreamConnection as the script-layer HttpUpstreamConnection.
// Holds the pool lease (and any transient connection); released when this object is destroyed.
class ConnectedUpstreamConnection final : public fiber::http_script::HttpUpstreamConnection {
public:
    explicit ConnectedUpstreamConnection(fiber::lite_nginx::upstream::AcquiredUpstreamConnection acquired) noexcept :
        acquired_(std::move(acquired)) {}

    [[nodiscard]] fiber::http::Http1ClientConnection &connection() noexcept override { return *acquired_.conn; }

private:
    fiber::lite_nginx::upstream::AcquiredUpstreamConnection acquired_;
};

} // namespace

HttpScriptServicesImpl::HttpScriptServicesImpl(upstream::UpstreamRegistry &upstreams, upstream::ConnectionPool &pool,
                                               DnsService &dns) noexcept :
    upstreams_(&upstreams), pool_(&pool), dns_(&dns) {}

fiber::async::Task<fiber::common::IoResult<std::unique_ptr<fiber::http_script::HttpUpstreamConnection>>>
HttpScriptServicesImpl::acquire(const fiber::http_script::HttpTargetSpec &target,
                                std::chrono::milliseconds connect_timeout) noexcept {
    using OutPtr = std::unique_ptr<fiber::http_script::HttpUpstreamConnection>;

    if (target.kind == fiber::http_script::HttpTargetSpec::Kind::Upstream) {
        const auto *peer = upstreams_->select_by_name(target.name);
        if (peer == nullptr || !peer->connection_key.has_value()) {
            co_return std::unexpected(fiber::common::IoErr::NotFound);
        }
        // For HTTPS upstreams, SNI uses the configured host (IP-literal peers have no name to send).
        const std::string_view sni =
                peer->connection_key->is_name() ? peer->connection_key->host_name() : std::string_view{};
        auto acquired = co_await fiber::lite_nginx::upstream::acquire_and_connect(*pool_, *dns_, *peer->connection_key,
                                                                                  sni, connect_timeout);
        if (!acquired) {
            co_return std::unexpected(acquired.error());
        }
        co_return OutPtr{new ConnectedUpstreamConnection(std::move(*acquired))};
    }

    // Url target: build a key from host:port:scheme. IP-literal host -> from_ip; hostname -> from_name.
    const std::uint16_t port = target.port != 0 ? target.port : static_cast<std::uint16_t>(target.tls ? 443 : 80);
    const auto scheme = target.tls ? fiber::http::Http1ConnectionGroupKey::Scheme::Https
                                   : fiber::http::Http1ConnectionGroupKey::Scheme::Http;

    fiber::net::IpAddress ip;
    std::optional<fiber::http::Http1ConnectionGroupKey> key;
    if (fiber::net::IpAddress::parse(target.name, ip)) {
        key = fiber::http::Http1ConnectionGroupKey::from_ip(ip, port, scheme);
    } else {
        key = fiber::http::Http1ConnectionGroupKey::from_name(target.name, port, scheme);
        if (!key) {
            co_return std::unexpected(fiber::common::IoErr::Invalid);
        }
    }

    // SNI = the host as given in the URL for HTTPS.
    auto acquired = co_await fiber::lite_nginx::upstream::acquire_and_connect(*pool_, *dns_, *key, target.name,
                                                                              connect_timeout);
    if (!acquired) {
        co_return std::unexpected(acquired.error());
    }
    co_return OutPtr{new ConnectedUpstreamConnection(std::move(*acquired))};
}

} // namespace fiber::lite_nginx::runtime
