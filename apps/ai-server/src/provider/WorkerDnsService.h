#ifndef FIBER_AI_SERVER_WORKER_DNS_SERVICE_H
#define FIBER_AI_SERVER_WORKER_DNS_SERVICE_H

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <async/Task.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <dns/DnsCache2.h>
#include <dns/DnsResolver.h>
#include <net/IpAddress.h>

namespace fiber::event {
class EventLoop;
class EventLoopGroup;
} // namespace fiber::event

namespace fiber::ai_server {

struct ProviderResolvedAddresses {
    static constexpr std::size_t kCapacity = 16;

    std::array<net::IpAddress, kCapacity> addresses{};
    std::uint16_t size = 0;
};

class WorkerDnsService final : public common::NonCopyable, public common::NonMovable {
public:
    WorkerDnsService() noexcept = default;
    ~WorkerDnsService();

    [[nodiscard]] async::Task<bool> init(event::EventLoopGroup &group) noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] async::Task<common::IoResult<ProviderResolvedAddresses>> resolve(std::string_view host) noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

private:
    struct LoopEntry {
        event::EventLoop *loop = nullptr;
        std::unique_ptr<dns::DnsResolverLocal> local;
        std::unique_ptr<dns::DnsResolver> resolver;
    };

    dns::SharedDnsCache2 cache_;
    event::EventLoop *cache_loop_ = nullptr;
    std::vector<LoopEntry> entries_;
    bool initialized_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_WORKER_DNS_SERVICE_H
