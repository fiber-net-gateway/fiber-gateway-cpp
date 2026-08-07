#include "AccessDnsService.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <string>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/net/SocketAddress.h>

namespace fiber::access_server {
namespace {

net::SocketAddress read_nameserver() noexcept {
    std::ifstream file("/etc/resolv.conf");
    std::string line;
    while (std::getline(file, line)) {
        std::string_view view = line;
        while (!view.empty() && (view.front() == ' ' || view.front() == '\t')) {
            view.remove_prefix(1);
        }
        constexpr std::string_view kNameserver = "nameserver";
        if (!view.starts_with(kNameserver)) {
            continue;
        }
        view.remove_prefix(kNameserver.size());
        while (!view.empty() && (view.front() == ' ' || view.front() == '\t')) {
            view.remove_prefix(1);
        }
        const std::size_t end = view.find_first_of(" \t#");
        const std::string_view text = view.substr(0, end);
        net::IpAddress address;
        if (!text.empty() && net::IpAddress::parse(text, address)) {
            return net::SocketAddress(address, 53);
        }
    }
    return net::SocketAddress(net::IpAddress::v4({8, 8, 8, 8}), 53);
}

} // namespace

AccessDnsService::~AccessDnsService() { FIBER_ASSERT(!initialized_); }

bool AccessDnsService::init(event::EventLoopGroup &group) noexcept {
    if (initialized_) {
        return true;
    }
    if (group.size() == 0) {
        return false;
    }
    cache_loop_ = &group.at(0);
    if (!cache_.init(*cache_loop_)) {
        cache_loop_ = nullptr;
        return false;
    }
    initialized_ = true;

    const net::SocketAddress nameserver = read_nameserver();
    entries_.reserve(group.size());
    for (std::size_t i = 0; i < group.size(); ++i) {
        LoopEntry entry;
        entry.loop = &group.at(i);
        entry.local = std::make_unique<dns::DnsResolverLocal>();
        dns::DnsClient::Options client_options;
        client_options.server = nameserver;
        client_options.timeout = std::chrono::milliseconds(2000);
        client_options.attempts = 2;
        if (!entry.local->init(*entry.loop, cache_, client_options)) {
            shutdown();
            return false;
        }
        entry.resolver = std::make_unique<dns::DnsResolver>();
        if (!entry.resolver->init(*entry.local)) {
            entries_.push_back(std::move(entry));
            shutdown();
            return false;
        }
        entries_.push_back(std::move(entry));
    }
    return true;
}

void AccessDnsService::shutdown() noexcept {
    if (!initialized_) {
        return;
    }
    if (!entries_.empty()) {
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        auto remaining = std::make_shared<std::atomic<std::size_t>>(entries_.size());
        for (LoopEntry &entry: entries_) {
            async::spawn(*entry.loop, [&entry, remaining, done]() -> async::DetachedTask {
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
    if (cache_loop_) {
        auto done = std::make_shared<std::promise<void>>();
        auto future = done->get_future();
        async::spawn(*cache_loop_, [this, done]() -> async::DetachedTask {
            cache_.shutdown();
            done->set_value();
            co_return;
        });
        future.wait();
    }
    cache_loop_ = nullptr;
    initialized_ = false;
}

ProxyDnsResolver AccessDnsService::adapter() noexcept {
    return ProxyDnsResolver{
            .context = this,
            .resolve = &AccessDnsService::resolve,
    };
}

async::Task<common::IoResult<std::vector<net::IpAddress>>> AccessDnsService::resolve(void *context,
                                                                                     std::string_view host) noexcept {
    auto &self = *static_cast<AccessDnsService *>(context);
    event::EventLoop *current = event::EventLoop::current_or_null();
    dns::DnsResolver *resolver = nullptr;
    for (const LoopEntry &entry: self.entries_) {
        if (entry.loop == current) {
            resolver = entry.resolver.get();
            break;
        }
    }
    if (!resolver) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    dns::AddressResolveResult result;
    if (!result.init()) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    auto resolved = co_await resolver->resolve_host(host, result);
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
