#include "DnsService.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "async/Spawn.h"
#include "async/Task.h"
#include "event/EventLoop.h"
#include "event/EventLoopGroup.h"
#include "net/IpAddress.h"
#include "net/SocketAddress.h"

namespace fiber::lite_nginx::runtime {
namespace {

// Reads the first "nameserver <ip>" from /etc/resolv.conf. Falls back to 8.8.8.8 when absent.
fiber::net::SocketAddress read_nameserver() noexcept {
    std::ifstream file("/etc/resolv.conf");
    std::string line;
    while (std::getline(file, line)) {
        // skip comments / whitespace
        std::size_t pos = 0;
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
            ++pos;
        }
        if (pos >= line.size() || line[pos] == '#') {
            continue;
        }
        constexpr std::string_view kNs = "nameserver";
        if (line.size() - pos < kNs.size() || line.compare(pos, kNs.size(), kNs) != 0) {
            continue;
        }
        pos += kNs.size();
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
            ++pos;
        }
        std::size_t start = pos;
        while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t' && line[pos] != '#') {
            ++pos;
        }
        const std::string ip_text = line.substr(start, pos - start);
        fiber::net::IpAddress ip;
        if (!ip_text.empty() && fiber::net::IpAddress::parse(ip_text, ip)) {
            return fiber::net::SocketAddress(ip, 53);
        }
    }
    return fiber::net::SocketAddress(fiber::net::IpAddress::v4({8, 8, 8, 8}), 53);
}

} // namespace

DnsService::~DnsService() { shutdown(); }

bool DnsService::init(fiber::event::EventLoopGroup &group) noexcept {
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

    const fiber::net::SocketAddress nameserver = read_nameserver();
    const std::size_t n = group.size();
    entries_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        LoopEntry entry;
        entry.loop = &group.at(i);

        entry.local = std::make_unique<fiber::dns::DnsResolverLocal>();
        fiber::dns::DnsClient::Options client_options{};
        client_options.server = nameserver;
        client_options.timeout = std::chrono::milliseconds(2000);
        client_options.attempts = 2;
        if (!entry.local->init(*entry.loop, cache_, client_options)) {
            shutdown();
            return false;
        }

        entry.resolver = std::make_unique<fiber::dns::DnsResolver>();
        if (!entry.resolver->init(*entry.local)) {
            entries_.push_back(std::move(entry));
            shutdown();
            return false;
        }

        entries_.push_back(std::move(entry));
    }
    if (entries_.empty()) {
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
