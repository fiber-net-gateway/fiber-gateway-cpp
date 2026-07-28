#ifndef FIBER_AI_SERVER_WORKER_DNS_SERVICE_H
#define FIBER_AI_SERVER_WORKER_DNS_SERVICE_H

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <async/Task.h>
#include <common/IoError.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>
#include <dns/DnsCache2.h>
#include <dns/DnsResolver.h>
#include <net/IpAddress.h>
#include <net/SocketAddress.h>

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

struct ProviderDnsError {
    common::IoErr io_error = common::IoErr::None;
    bool backoff_hit = false;
};

class WorkerDnsService final : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::optional<net::SocketAddress> nameserver;
        std::chrono::milliseconds timeout{2000};
        std::chrono::milliseconds transient_failure_ttl{2000};
        std::uint8_t attempts = 2;
    };

    WorkerDnsService() noexcept = default;
    explicit WorkerDnsService(Options options) noexcept : options_(std::move(options)) {}
    ~WorkerDnsService();

    [[nodiscard]] async::Task<bool> init(event::EventLoopGroup &group) noexcept;
    [[nodiscard]] async::Task<void> shutdown() noexcept;

    [[nodiscard]] async::Task<std::expected<ProviderResolvedAddresses, ProviderDnsError>>
    resolve(std::string_view host) noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

private:
    class TransientFailureCache {
    public:
        struct LookupResult {
            std::uint64_t generation = 0;
            bool backoff_hit = false;
        };

        [[nodiscard]] LookupResult lookup(std::string_view host, std::chrono::steady_clock::time_point now) noexcept;
        void record_timeout(std::string_view host, std::uint64_t generation,
                            std::chrono::steady_clock::time_point expires_at) noexcept;
        void record_success(std::string_view host) noexcept;
        void clear() noexcept;

    private:
        static constexpr std::size_t kCapacity = 64;
        static constexpr std::size_t kMaxHostBytes = 255;

        struct Entry {
            std::array<char, kMaxHostBytes> host{};
            std::chrono::steady_clock::time_point expires_at{};
            std::uint64_t generation = 0;
            std::uint16_t size = 0;
        };

        [[nodiscard]] static std::uint16_t normalize(std::string_view host,
                                                     std::array<char, kMaxHostBytes> &output) noexcept;
        [[nodiscard]] std::uint64_t allocate_generation() noexcept;

        std::array<Entry, kCapacity> entries_{};
        std::mutex mutex_;
        std::uint64_t next_generation_ = 1;
    };

    struct LoopEntry {
        event::EventLoop *loop = nullptr;
        std::unique_ptr<dns::DnsResolverLocal> local;
        std::unique_ptr<dns::DnsResolver> resolver;
    };

    Options options_;
    TransientFailureCache transient_failures_;
    dns::SharedDnsCache2 cache_;
    event::EventLoop *cache_loop_ = nullptr;
    std::vector<LoopEntry> entries_;
    bool initialized_ = false;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_WORKER_DNS_SERVICE_H
