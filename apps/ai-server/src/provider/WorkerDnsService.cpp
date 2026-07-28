#include "WorkerDnsService.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>

#include <async/Spawn.h>
#include <async/WaitGroup.h>
#include <common/Assert.h>
#include <dns/DnsClient.h>
#include <event/EventLoop.h>
#include <event/EventLoopGroup.h>
#include <net/SocketAddress.h>

namespace fiber::ai_server {
namespace {

template<typename Factory>
void spawn_on(event::EventLoop &loop, Factory &&factory) {
    if (event::EventLoop::current_or_null() == &loop) {
        async::spawn(std::forward<Factory>(factory));
        return;
    }
    async::spawn(loop, std::forward<Factory>(factory));
}

net::SocketAddress read_nameserver() noexcept {
    std::ifstream file("/etc/resolv.conf");
    std::string line;
    while (std::getline(file, line)) {
        std::size_t pos = 0;
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
            ++pos;
        }
        if (pos >= line.size() || line[pos] == '#') {
            continue;
        }
        constexpr std::string_view kNameserver = "nameserver";
        if (line.size() - pos < kNameserver.size() || line.compare(pos, kNameserver.size(), kNameserver) != 0) {
            continue;
        }
        pos += kNameserver.size();
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
            ++pos;
        }
        const std::size_t start = pos;
        while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t' && line[pos] != '#') {
            ++pos;
        }
        net::IpAddress ip;
        if (pos > start && net::IpAddress::parse(std::string_view(line).substr(start, pos - start), ip)) {
            return net::SocketAddress(ip, 53);
        }
    }
    return net::SocketAddress(net::IpAddress::v4({8, 8, 8, 8}), 53);
}

} // namespace

std::uint16_t WorkerDnsService::TransientFailureCache::normalize(std::string_view host,
                                                                 std::array<char, kMaxHostBytes> &output) noexcept {
    while (!host.empty() && host.back() == '.') {
        host.remove_suffix(1);
    }
    if (host.empty() || host.size() > output.size()) {
        return 0;
    }
    for (std::size_t i = 0; i < host.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(host[i]);
        output[i] = ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : static_cast<char>(ch);
    }
    return static_cast<std::uint16_t>(host.size());
}

std::uint64_t WorkerDnsService::TransientFailureCache::allocate_generation() noexcept {
    std::uint64_t generation = next_generation_++;
    if (generation == 0) {
        generation = next_generation_++;
    }
    return generation;
}

WorkerDnsService::TransientFailureCache::LookupResult
WorkerDnsService::TransientFailureCache::lookup(std::string_view host,
                                                std::chrono::steady_clock::time_point now) noexcept {
    std::array<char, kMaxHostBytes> normalized{};
    const std::uint16_t size = normalize(host, normalized);
    if (size == 0) {
        return {};
    }

    std::lock_guard lock(mutex_);
    Entry *target = nullptr;
    for (Entry &entry: entries_) {
        if (entry.size == size && std::memcmp(entry.host.data(), normalized.data(), size) == 0) {
            const bool backoff_hit =
                    entry.expires_at != std::chrono::steady_clock::time_point{} && entry.expires_at > now;
            if (!backoff_hit) {
                entry.expires_at = {};
                entry.generation = allocate_generation();
            }
            return LookupResult{
                    .generation = entry.generation,
                    .backoff_hit = backoff_hit,
            };
        }
        if (!target || entry.size == 0 || entry.expires_at < target->expires_at) {
            target = &entry;
        }
    }
    FIBER_ASSERT(target != nullptr);
    std::memcpy(target->host.data(), normalized.data(), size);
    target->expires_at = {};
    target->generation = allocate_generation();
    target->size = size;
    return LookupResult{.generation = target->generation};
}

void WorkerDnsService::TransientFailureCache::record_timeout(
        std::string_view host, std::uint64_t generation, std::chrono::steady_clock::time_point expires_at) noexcept {
    if (generation == 0) {
        return;
    }
    std::array<char, kMaxHostBytes> normalized{};
    const std::uint16_t size = normalize(host, normalized);
    if (size == 0) {
        return;
    }

    std::lock_guard lock(mutex_);
    for (Entry &entry: entries_) {
        if (entry.size == size && entry.generation == generation &&
            std::memcmp(entry.host.data(), normalized.data(), size) == 0) {
            entry.expires_at = std::max(entry.expires_at, expires_at);
            return;
        }
    }
}

void WorkerDnsService::TransientFailureCache::record_success(std::string_view host) noexcept {
    std::array<char, kMaxHostBytes> normalized{};
    const std::uint16_t size = normalize(host, normalized);
    if (size == 0) {
        return;
    }

    std::lock_guard lock(mutex_);
    for (Entry &entry: entries_) {
        if (entry.size == size && std::memcmp(entry.host.data(), normalized.data(), size) == 0) {
            entry.expires_at = {};
            entry.generation = allocate_generation();
            return;
        }
    }
}

void WorkerDnsService::TransientFailureCache::clear() noexcept {
    std::lock_guard lock(mutex_);
    for (Entry &entry: entries_) {
        entry.size = 0;
    }
    next_generation_ = 1;
}

WorkerDnsService::~WorkerDnsService() {
    FIBER_ASSERT(!initialized_);
    FIBER_ASSERT(entries_.empty());
}

async::Task<bool> WorkerDnsService::init(event::EventLoopGroup &group) noexcept {
    if (initialized_) {
        co_return true;
    }
    if (group.size() == 0) {
        co_return false;
    }
    cache_loop_ = &group.at(0);
    std::atomic_bool cache_ready{false};
    async::WaitGroup cache_init;
    cache_init.add();
    spawn_on(*cache_loop_, [this, &cache_ready, &cache_init]() -> async::DetachedTask {
        cache_ready.store(cache_.init(*cache_loop_), std::memory_order_release);
        cache_init.done();
        co_return;
    });
    co_await cache_init.join();
    if (!cache_ready.load(std::memory_order_acquire)) {
        cache_loop_ = nullptr;
        co_return false;
    }
    initialized_ = true;

    const net::SocketAddress nameserver = options_.nameserver ? *options_.nameserver : read_nameserver();
    entries_.resize(group.size());
    for (std::size_t i = 0; i < group.size(); ++i) {
        entries_[i].loop = &group.at(i);
        entries_[i].local = std::make_unique<dns::DnsResolverLocal>();
        entries_[i].resolver = std::make_unique<dns::DnsResolver>();
    }

    std::atomic_bool all_ready{true};
    async::WaitGroup resolver_init;
    resolver_init.add(entries_.size());
    for (LoopEntry &entry: entries_) {
        LoopEntry *current = &entry;
        spawn_on(*current->loop, [this, current, nameserver, &all_ready, &resolver_init]() -> async::DetachedTask {
            dns::DnsClient::Options client_options;
            client_options.server = nameserver;
            client_options.timeout = options_.timeout;
            client_options.attempts = options_.attempts;
            const bool ready = current->local->init(*current->loop, cache_, client_options) &&
                               current->resolver->init(*current->local);
            if (!ready) {
                all_ready.store(false, std::memory_order_release);
            }
            resolver_init.done();
            co_return;
        });
    }
    co_await resolver_init.join();
    co_return all_ready.load(std::memory_order_acquire);
}

async::Task<void> WorkerDnsService::shutdown() noexcept {
    if (!initialized_) {
        co_return;
    }

    async::WaitGroup releases;
    releases.add(entries_.size());
    for (LoopEntry &entry: entries_) {
        LoopEntry *current = &entry;
        spawn_on(*current->loop, [current, &releases]() -> async::DetachedTask {
            if (current->resolver) {
                current->resolver->release();
                current->resolver.reset();
            }
            if (current->local) {
                current->local->release();
                current->local.reset();
            }
            releases.done();
            co_return;
        });
    }
    co_await releases.join();
    entries_.clear();

    async::WaitGroup cache_release;
    cache_release.add();
    spawn_on(*cache_loop_, [this, &cache_release]() -> async::DetachedTask {
        cache_.shutdown();
        cache_release.done();
        co_return;
    });
    co_await cache_release.join();
    transient_failures_.clear();
    cache_loop_ = nullptr;
    initialized_ = false;
}

async::Task<std::expected<ProviderResolvedAddresses, ProviderDnsError>>
WorkerDnsService::resolve(std::string_view host) noexcept {
    event::EventLoop *current = event::EventLoop::current_or_null();
    dns::DnsResolver *resolver = nullptr;
    for (const LoopEntry &entry: entries_) {
        if (entry.loop == current) {
            resolver = entry.resolver.get();
            break;
        }
    }
    if (!resolver) {
        co_return std::unexpected(ProviderDnsError{.io_error = common::IoErr::Invalid});
    }
    const bool transient_enabled = options_.transient_failure_ttl > std::chrono::milliseconds::zero();
    TransientFailureCache::LookupResult transient;
    if (transient_enabled) {
        transient = transient_failures_.lookup(host, current->now());
        if (transient.backoff_hit) {
            co_return std::unexpected(ProviderDnsError{
                    .io_error = common::IoErr::TimedOut,
                    .backoff_hit = true,
            });
        }
    }

    dns::AddressResolveResult resolved;
    if (!resolved.init({
                .max_records = ProviderResolvedAddresses::kCapacity,
                .max_name_storage = 512,
        })) {
        co_return std::unexpected(ProviderDnsError{.io_error = common::IoErr::NoMem});
    }
    auto result = co_await resolver->resolve_host(host, resolved);
    if (!result) {
        if (result.error() == common::IoErr::TimedOut && transient_enabled) {
            transient_failures_.record_timeout(host, transient.generation,
                                               event::EventLoop::current().now() + options_.transient_failure_ttl);
        }
        co_return std::unexpected(ProviderDnsError{.io_error = result.error()});
    }
    if (transient_enabled) {
        transient_failures_.record_success(host);
    }
    if (resolved.record_count() == 0) {
        co_return std::unexpected(ProviderDnsError{.io_error = common::IoErr::NotFound});
    }

    ProviderResolvedAddresses output;
    output.size = resolved.record_count();
    for (std::uint16_t i = 0; i < output.size; ++i) {
        output.addresses[i] = resolved.records()[i];
    }
    co_return output;
}

} // namespace fiber::ai_server
