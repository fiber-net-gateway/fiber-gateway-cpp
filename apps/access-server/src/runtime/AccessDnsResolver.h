#ifndef FIBER_ACCESS_SERVER_ACCESS_DNS_RESOLVER_H
#define FIBER_ACCESS_SERVER_ACCESS_DNS_RESOLVER_H

#include "../execution/ProxyRequestSender.h"

#include <dns/DnsResolver.h>

namespace fiber::access_server {

class AccessDnsResolver final {
public:
    explicit AccessDnsResolver(dns::DnsResolver &resolver) noexcept : resolver_(&resolver) {}

    [[nodiscard]] ProxyDnsResolver adapter() noexcept {
        return ProxyDnsResolver{
                .context = this,
                .resolve = &AccessDnsResolver::resolve,
        };
    }

private:
    [[nodiscard]] static async::Task<common::IoResult<std::vector<net::IpAddress>>>
    resolve(void *context, std::string_view host) noexcept;

    dns::DnsResolver *resolver_ = nullptr;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_DNS_RESOLVER_H
