#include <fiber/cat/CatClientConfig.h>

#include <utility>

#include <net/IpAddress.h>

namespace fiber::cat {

std::expected<CatClientConfig, CatConfigError> CatClientConfig::create(CatClientConfigParams params) {
    if (params.app_key.empty()) {
        return std::unexpected(CatConfigError::EmptyAppKey);
    }
    if (params.hostname.empty()) {
        return std::unexpected(CatConfigError::EmptyHostname);
    }
    if (params.ip.empty()) {
        return std::unexpected(CatConfigError::EmptyIp);
    }
    net::IpAddress client_ip;
    if (!net::IpAddress::parse(params.ip, client_ip) || client_ip.is_unspecified() || client_ip.is_multicast()) {
        return std::unexpected(CatConfigError::InvalidIp);
    }
    if (params.routers.empty() && params.bootstrap_collectors.empty()) {
        return std::unexpected(CatConfigError::EmptyServerList);
    }
    for (const CatRouterEndpoint &router: params.routers) {
        net::IpAddress literal;
        if (router.host.empty() || router.port == 0 ||
            (net::IpAddress::parse(router.host, literal) && literal.is_unspecified())) {
            return std::unexpected(CatConfigError::InvalidRouter);
        }
    }
    for (const net::SocketAddress &collector: params.bootstrap_collectors) {
        if (collector.port() == 0 || collector.ip().is_unspecified()) {
            return std::unexpected(CatConfigError::InvalidCollector);
        }
    }
    return CatClientConfig(std::move(params));
}

} // namespace fiber::cat
