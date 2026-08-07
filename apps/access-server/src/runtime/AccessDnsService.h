#ifndef FIBER_ACCESS_SERVER_ACCESS_DNS_SERVICE_H
#define FIBER_ACCESS_SERVER_ACCESS_DNS_SERVICE_H

#include "../execution/ProxyUpstreamConnection.h"

#include <memory>
#include <string_view>
#include <vector>

#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/dns/DnsCache2.h>
#include <fiber/dns/DnsResolver.h>

namespace fiber::event {
class EventLoop;
class EventLoopGroup;
} // namespace fiber::event

namespace fiber::access_server {

// DnsResolver is loop-affine. This service creates one resolver stack per
// request worker and shares only the thread-safe cache between those stacks.
class AccessDnsService final : public common::NonCopyable, public common::NonMovable {
public:
    AccessDnsService() noexcept = default;
    ~AccessDnsService();

    [[nodiscard]] bool init(event::EventLoopGroup &group) noexcept;
    void shutdown() noexcept;
    [[nodiscard]] ProxyDnsResolver adapter() noexcept;

private:
    struct LoopEntry {
        event::EventLoop *loop = nullptr;
        std::unique_ptr<dns::DnsResolverLocal> local;
        std::unique_ptr<dns::DnsResolver> resolver;
    };

    [[nodiscard]] static async::Task<common::IoResult<std::vector<net::IpAddress>>>
    resolve(void *context, std::string_view host) noexcept;

    dns::SharedDnsCache2 cache_;
    event::EventLoop *cache_loop_ = nullptr;
    std::vector<LoopEntry> entries_;
    bool initialized_ = false;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_DNS_SERVICE_H
