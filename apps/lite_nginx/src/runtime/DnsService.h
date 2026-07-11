#ifndef FIBER_LITE_NGINX_RUNTIME_DNS_SERVICE_H
#define FIBER_LITE_NGINX_RUNTIME_DNS_SERVICE_H

#include <memory>
#include <string_view>
#include <vector>

#include "async/Task.h"
#include "common/IoError.h"
#include "common/NonCopyable.h"
#include "common/NonMovable.h"
#include "dns/DnsCache.h"
#include "dns/DnsResolver.h"
#include "net/IpAddress.h"

namespace fiber::event {
class EventLoopGroup;
class EventLoop;
} // namespace fiber::event

namespace fiber::lite_nginx::runtime {

// Per-worker-loop DNS resolver stack sharing one cache (with singleflight). Each worker loop
// gets its own DnsResolverLocal + DnsResolver (resolve_host asserts it runs on its own loop),
// so http.request({url:"http://host"}) calls resolve on the calling worker's resolver.
//
// The upstream nameserver is read from /etc/resolv.conf (first nameserver line), defaulting to
// 8.8.8.8:53 when absent or unparseable.
class DnsService : public fiber::common::NonCopyable, public fiber::common::NonMovable {
public:
    DnsService() noexcept = default;
    ~DnsService();

    [[nodiscard]] bool init(fiber::event::EventLoopGroup &group) noexcept;
    void shutdown() noexcept;

    // Resolves host to its A/AAAA addresses using the calling worker loop's resolver. Must be
    // called from a worker loop that was part of the EventLoopGroup passed to init(). Returns all
    // records ordered by the resolver policy (V6First: AAAA then A); callers should dial them in
    // order and fall back to the next on connect failure.
    [[nodiscard]] fiber::async::Task<fiber::common::IoResult<std::vector<fiber::net::IpAddress>>>
    resolve(std::string_view host) noexcept;

private:
    struct LoopEntry {
        fiber::event::EventLoop *loop = nullptr;
        std::unique_ptr<fiber::dns::DnsResolverLocal> local;
        std::unique_ptr<fiber::dns::DnsResolver> resolver;
    };

    fiber::dns::SharedDnsCache cache_{};
    std::vector<LoopEntry> entries_{};
    bool initialized_ = false;
};

} // namespace fiber::lite_nginx::runtime

#endif // FIBER_LITE_NGINX_RUNTIME_DNS_SERVICE_H
