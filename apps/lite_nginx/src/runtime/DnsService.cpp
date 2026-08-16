#include "DnsService.h"

#include <atomic>
#include <future>
#include <memory>
#include <utility>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>

namespace fiber::lite_nginx::runtime {

DnsService::~DnsService() { shutdown(); }

bool DnsService::init(fiber::event::EventLoopGroup &group,
                      const fiber::dns::SystemResolverConfig &resolver_config) noexcept {
    if (initialized_) {
        return true;
    }
    if (group.size() == 0 || !group.running() || resolver_config.nameservers.empty() ||
        fiber::event::EventLoop::current_or_null() != nullptr) {
        return false;
    }
    cache_loop_ = &group.at(0);
    if (!cache_.init(*cache_loop_)) {
        cache_loop_ = nullptr;
        return false;
    }
    initialized_ = true;

    const std::size_t n = group.size();
    entries_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        LoopEntry entry;
        entry.loop = &group.at(i);
        entry.local = std::make_unique<fiber::dns::DnsResolverLocal>();
        entry.resolver = std::make_unique<fiber::dns::DnsResolver>();
        entries_.push_back(std::move(entry));
    }

    fiber::dns::DnsClient::Options client_options{};
    client_options.nameservers = resolver_config.nameservers;
    client_options.timeout = resolver_config.timeout;
    client_options.attempts = resolver_config.attempts;
    client_options.rotate_nameservers = resolver_config.rotate;

    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    auto remaining = std::make_shared<std::atomic<std::size_t>>(entries_.size());
    auto success = std::make_shared<std::atomic<bool>>(true);
    for (LoopEntry &entry: entries_) {
        fiber::async::spawn(*entry.loop,
                            [this, &entry, client_options, remaining, success, done]() -> fiber::async::DetachedTask {
                                const bool initialized = entry.local->init(*entry.loop, cache_, client_options) &&
                                                         entry.resolver->init(*entry.local);
                                if (!initialized) {
                                    success->store(false, std::memory_order_release);
                                }
                                if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                                    done->set_value();
                                }
                                co_return;
                            });
    }
    future.wait();
    if (!success->load(std::memory_order_acquire)) {
        shutdown();
        return false;
    }
    return true;
}

void DnsService::shutdown() noexcept {
    if (!initialized_) {
        return;
    }
    // DnsResolverLocal::release()/close() assert they run on the bound loop, so release each
    // loop's resolver+local on that loop, then clear.
    if (!entries_.empty()) {
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        auto remaining = std::make_shared<std::atomic<std::size_t>>(entries_.size());
        for (LoopEntry &entry: entries_) {
            fiber::async::spawn(*entry.loop, [&entry, remaining, done]() -> fiber::async::DetachedTask {
                if (entry.resolver) {
                    entry.resolver->release();
                    entry.resolver.reset();
                }
                if (entry.local) {
                    entry.local->release();
                    entry.local.reset();
                }
                if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    done->set_value();
                }
                co_return;
            });
        }
        future.wait();
    }
    entries_.clear();
    if (cache_loop_ != nullptr) {
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        fiber::async::spawn(*cache_loop_, [this, done]() -> fiber::async::DetachedTask {
            cache_.shutdown();
            done->set_value();
            co_return;
        });
        future.wait();
    }
    cache_loop_ = nullptr;
    initialized_ = false;
}

fiber::async::Task<fiber::common::IoResult<std::vector<fiber::net::IpAddress>>>
DnsService::resolve(std::string_view host) noexcept {
    fiber::event::EventLoop *current = fiber::event::EventLoop::current_or_null();
    fiber::dns::DnsResolver *resolver = nullptr;
    for (const LoopEntry &entry: entries_) {
        if (entry.loop == current) {
            resolver = entry.resolver.get();
            break;
        }
    }
    if (resolver == nullptr) {
        co_return std::unexpected(fiber::common::IoErr::Invalid);
    }

    fiber::dns::AddressResolveResult result;
    if (!result.init()) {
        co_return std::unexpected(fiber::common::IoErr::NoMem);
    }
    auto resolve_result = co_await resolver->resolve_host(host, result);
    if (!resolve_result) {
        co_return std::unexpected(resolve_result.error());
    }
    const std::uint16_t count = result.record_count();
    if (count == 0) {
        co_return std::unexpected(fiber::common::IoErr::NotFound);
    }
    std::vector<fiber::net::IpAddress> addresses;
    addresses.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        addresses.push_back(result.records()[i]);
    }
    co_return addresses;
}

} // namespace fiber::lite_nginx::runtime
