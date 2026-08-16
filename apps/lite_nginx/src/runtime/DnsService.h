#ifndef FIBER_LITE_NGINX_RUNTIME_DNS_SERVICE_H
#define FIBER_LITE_NGINX_RUNTIME_DNS_SERVICE_H

#include <memory>
#include <string_view>
#include <vector>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/dns/DnsCache2.h>
#include <fiber/dns/DnsResolver.h>
#include <fiber/dns/DnsResolverConfig.h>
#include <fiber/net/IpAddress.h>

namespace fiber::event {
class EventLoopGroup;
class EventLoop;
} // namespace fiber::event

namespace fiber::lite_nginx::runtime {

// Per-worker-loop DNS resolver stack sharing one cache (with singleflight). Each worker loop
// gets its own DnsResolverLocal + DnsResolver (resolve_host asserts it runs on its own loop),
// so a directive-bound http(s)://host target resolves host on the calling worker's resolver.
//
// System resolver configuration is loaded before worker loops start and injected here. Every
// worker gets the same bounded nameserver list and retry policy.
class DnsService : public fiber::common::NonCopyable, public fiber::common::NonMovable {
public:
    DnsService() noexcept = default;
    ~DnsService();

    [[nodiscard]] bool init(fiber::event::EventLoopGroup &group,
                            const fiber::dns::SystemResolverConfig &resolver_config) noexcept;
    void shutdown() noexcept;

    // Resolves host to its A/AAAA addresses using the calling worker loop's resolver. Must be
    // called from a worker loop that was part of the EventLoopGroup passed to init(). Returns all
    // records ordered by the resolver policy (V6First: AAAA then A). Connection callers may apply
    // deterministic family interleaving after resolution.
    [[nodiscard]] fiber::async::Task<fiber::common::IoResult<std::vector<fiber::net::IpAddress>>>
    resolve(std::string_view host) noexcept;

private:
    struct LoopEntry {
        fiber::event::EventLoop *loop = nullptr;
        std::unique_ptr<fiber::dns::DnsResolverLocal> local;
        std::unique_ptr<fiber::dns::DnsResolver> resolver;
    };

    fiber::dns::SharedDnsCache2 cache_{};
    fiber::event::EventLoop *cache_loop_ = nullptr;
    std::vector<LoopEntry> entries_{};
    bool initialized_ = false;
};

} // namespace fiber::lite_nginx::runtime

#endif // FIBER_LITE_NGINX_RUNTIME_DNS_SERVICE_H
