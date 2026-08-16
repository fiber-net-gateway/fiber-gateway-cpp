#ifndef FIBER_NACOS_TESTS_NACOS_TEST_DNS_H
#define FIBER_NACOS_TESTS_NACOS_TEST_DNS_H

#include <chrono>
#include <string_view>

#include <fiber/common/IoError.h>
#include <fiber/dns/DnsCache2.h>
#include <fiber/dns/DnsClient.h>
#include <fiber/dns/DnsResolver.h>
#include <fiber/dns/DnsResolverLocal.h>
#include <fiber/event/EventLoop.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>

namespace fiber::nacos::test {

class NacosTestDns {
public:
    [[nodiscard]] bool init(event::EventLoop &loop, std::string_view normalized_host) noexcept {
        if (!cache_.init(loop)) {
            return false;
        }
        cache_initialized_ = true;

        dns::DnsClient::Options client_options;
        (void) client_options.nameservers.add(net::SocketAddress(net::IpAddress::loopback_v4(), 65053));
        client_options.timeout = std::chrono::milliseconds(100);
        client_options.attempts = 1;
        if (!local_.init(loop, cache_, client_options) ||
            !resolver_.init(local_, dns::DnsResolver::Options{.default_policy = dns::AddressPolicy::V4First}) ||
            !address_resolver_.init(resolver_)) {
            release();
            return false;
        }

        const net::IpAddress addresses[]{
                net::IpAddress::v4({127, 0, 0, 2}),
                net::IpAddress::loopback_v4(),
        };
        const dns::DnsCacheKey key{normalized_host, dns::dns_cache_hash(normalized_host)};
        const auto expire_at = loop.now() + std::chrono::seconds(60);
        const common::IoErr v4 =
                cache_.upsert_address_set(key, net::IpFamily::V4, addresses, std::size(addresses), expire_at);
        const common::IoErr v6 = cache_.upsert_address_set(key, net::IpFamily::V6, nullptr, 0, expire_at);
        return v4 == common::IoErr::None && v6 == common::IoErr::None;
    }

    void release() noexcept {
        address_resolver_.release();
        resolver_.release();
        local_.release();
        if (cache_initialized_) {
            cache_.shutdown();
            cache_initialized_ = false;
        }
    }

    [[nodiscard]] dns::AddressResolver &address_resolver() noexcept { return address_resolver_; }

private:
    dns::SharedDnsCache2 cache_;
    dns::DnsResolverLocal local_;
    dns::DnsResolver resolver_;
    dns::AddressResolver address_resolver_;
    bool cache_initialized_ = false;
};

} // namespace fiber::nacos::test

#endif // FIBER_NACOS_TESTS_NACOS_TEST_DNS_H
