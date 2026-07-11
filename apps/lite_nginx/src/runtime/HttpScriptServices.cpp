#include "HttpScriptServices.h"

#include <utility>

#include "net/IpAddress.h"
#include "net/SocketAddress.h"
#include "net/TlsOptions.h"

#include "../upstream/UpstreamRegistry.h"

namespace fiber::lite_nginx::runtime {
namespace {

// Wraps an UpstreamRegistry::ConnectionHandle as the script-layer HttpUpstreamConnection. The
// handle (and its pool lease) is released when this object is destroyed.
class LiteNginxHttpUpstreamConnection : public fiber::http_script::HttpUpstreamConnection {
public:
    explicit LiteNginxHttpUpstreamConnection(upstream::UpstreamRegistry::ConnectionHandle handle) noexcept :
        handle_(std::move(handle)) {}

    [[nodiscard]] fiber::net::SocketAddress peer_addr() const noexcept override { return handle_.peer_addr; }
    [[nodiscard]] fiber::net::TlsOptions tls() const noexcept override { return handle_.tls; }
    [[nodiscard]] bool pooled() const noexcept override { return handle_.pooled(); }
    [[nodiscard]] fiber::http::Http1ClientConnection *connection() noexcept override { return handle_.lease.get(); }
    [[nodiscard]] fiber::common::IoResult<fiber::http::Http1ClientConnection *>
    emplace_connection(fiber::http::Http1ClientConnectionOptions options) noexcept override {
        if (!handle_.pooled()) {
            return std::unexpected(fiber::common::IoErr::Invalid);
        }
        return handle_.lease.emplace_connection(std::move(options));
    }

private:
    upstream::UpstreamRegistry::ConnectionHandle handle_;
};

} // namespace

HttpScriptServicesImpl::HttpScriptServicesImpl(upstream::UpstreamRegistry &upstreams, DnsService &dns) noexcept :
    upstreams_(&upstreams), dns_(&dns) {}

fiber::async::Task<fiber::common::IoResult<std::unique_ptr<fiber::http_script::HttpUpstreamConnection>>>
HttpScriptServicesImpl::acquire(const fiber::http_script::HttpTargetSpec &target) noexcept {
    using OutPtr = std::unique_ptr<fiber::http_script::HttpUpstreamConnection>;

    if (target.kind == fiber::http_script::HttpTargetSpec::Kind::Upstream) {
        auto handle = co_await upstreams_->acquire_by_name(target.name);
        if (!handle.valid()) {
            co_return std::unexpected(fiber::common::IoErr::NotFound);
        }
        co_return OutPtr{new LiteNginxHttpUpstreamConnection(std::move(handle))};
    }

    // Url target.
    const std::uint16_t port = target.port != 0 ? target.port : static_cast<std::uint16_t>(target.tls ? 443 : 80);
    const auto scheme = target.tls ? fiber::http::Http1ConnectionGroupKey::Scheme::Https
                                   : fiber::http::Http1ConnectionGroupKey::Scheme::Http;

    fiber::net::IpAddress ip;
    if (!fiber::net::IpAddress::parse(target.name, ip)) {
        if (dns_ == nullptr) {
            co_return std::unexpected(fiber::common::IoErr::NotFound);
        }
        auto resolved = co_await dns_->resolve(target.name);
        if (!resolved) {
            co_return std::unexpected(resolved.error());
        }
        ip = *resolved;
    }

    const fiber::http::Http1ConnectionGroupKey key = fiber::http::Http1ConnectionGroupKey::from_ip(ip, port, scheme);
    const fiber::net::SocketAddress peer_addr(ip, port);
    fiber::net::TlsOptions tls_opts{};
    if (target.tls) {
        tls_opts.server_name = target.name;
    }

    auto handle = co_await upstreams_->acquire_by_key(key, peer_addr, std::move(tls_opts));
    if (!handle.valid()) {
        co_return std::unexpected(fiber::common::IoErr::NotFound);
    }
    co_return OutPtr{new LiteNginxHttpUpstreamConnection(std::move(handle))};
}

} // namespace fiber::lite_nginx::runtime
