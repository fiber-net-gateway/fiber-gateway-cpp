#include "AccessDnsResolver.h"

namespace fiber::access_server {

async::Task<common::IoResult<std::vector<net::IpAddress>>> AccessDnsResolver::resolve(void *context,
                                                                                      std::string_view host) noexcept {
    auto &self = *static_cast<AccessDnsResolver *>(context);
    dns::AddressResolveResult result;
    if (!result.init()) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    auto resolved = co_await self.resolver_->resolve_host(host, result);
    if (!resolved) {
        co_return std::unexpected(resolved.error());
    }
    if (result.record_count() == 0) {
        co_return std::unexpected(common::IoErr::NotFound);
    }

    std::vector<net::IpAddress> addresses;
    addresses.reserve(result.record_count());
    for (std::uint16_t i = 0; i < result.record_count(); ++i) {
        addresses.push_back(result.records()[i]);
    }
    co_return addresses;
}

} // namespace fiber::access_server
