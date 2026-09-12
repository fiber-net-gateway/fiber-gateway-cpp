#include "HttpScriptServices.h"

#include <string>
#include <utility>

#include <fiber/http/Http1ClientConnection.h>
#include <fiber/http_script/HttpScriptServices.h>
#include <fiber/http_script/HttpTarget.h>

#include "../upstream/ConnectionPool.h"
#include "../upstream/UpstreamConnection.h"
#include "../upstream/UpstreamRegistry.h"

namespace fiber::lite_nginx::runtime {
namespace {

// Wraps the unified AcquiredUpstreamConnection as the script-layer HttpUpstreamConnection.
// Holds the pool lease (and any transient connection); released when this object is destroyed.
class ConnectedUpstreamConnection final : public fiber::http_script::HttpUpstreamConnection {
public:
    ConnectedUpstreamConnection(fiber::lite_nginx::upstream::AcquiredUpstreamConnection acquired,
                                std::string host_header) noexcept :
        acquired_(std::move(acquired)), host_header_(std::move(host_header)) {}

    [[nodiscard]] fiber::http::Http1ClientConnection &connection() noexcept override { return *acquired_.conn; }
    [[nodiscard]] std::string_view host_header() const noexcept override { return host_header_; }

private:
    fiber::lite_nginx::upstream::AcquiredUpstreamConnection acquired_;
    std::string host_header_;
};

// `host[:port]` for a URL target, mirroring what a client puts in Host: IPv6 literals are
// bracketed and the scheme's default port is omitted.
std::string url_target_authority(const fiber::http_script::HttpTargetSpec &target) {
    const bool v6_literal = target.name.find(':') != std::string::npos;
    std::string authority;
    authority.reserve(target.name.size() + 8);
    if (v6_literal) {
        authority.push_back('[');
    }
    authority.append(target.name);
    if (v6_literal) {
        authority.push_back(']');
    }
    const std::uint16_t default_port = target.tls ? 443 : 80;
    if (target.port != 0 && target.port != default_port) {
        authority.push_back(':');
        authority.append(std::to_string(target.port));
    }
    return authority;
}

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
        auto acquired = co_await fiber::lite_nginx::upstream::acquire_and_connect(*pool_, *dns_, *peer->connection_key,
                                                                                  connect_timeout);
        if (!acquired) {
            co_return std::unexpected(acquired.error());
        }
        // Same default Host as `proxy_pass upstream://<name>`: the upstream's name.
        std::string_view upstream_name = target.name;
        if (!upstream_name.empty() && upstream_name.front() == '@') {
            upstream_name.remove_prefix(1);
        }
        co_return OutPtr{new ConnectedUpstreamConnection(std::move(*acquired), std::string(upstream_name))};
    }

    // Url target: the key is host:port:scheme; a literal host dials directly, a name resolves via
    // DNS. HttpTargetSpec::parse already rejected https:// with a literal host at directive bind.
    const std::uint16_t port = target.port != 0 ? target.port : static_cast<std::uint16_t>(target.tls ? 443 : 80);
    const auto scheme = target.tls ? fiber::http::HttpConnectionGroupKey::Scheme::Https
                                   : fiber::http::HttpConnectionGroupKey::Scheme::Http;
    auto key = fiber::http::HttpConnectionGroupKey::make(target.name, port, scheme);
    if (!key) {
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }

    auto acquired = co_await fiber::lite_nginx::upstream::acquire_and_connect(*pool_, *dns_, *key, connect_timeout);
    if (!acquired) {
        co_return std::unexpected(acquired.error());
    }
    co_return OutPtr{new ConnectedUpstreamConnection(std::move(*acquired), url_target_authority(target))};
}

} // namespace fiber::lite_nginx::runtime
