#ifndef FIBER_DNS_DNS_ADDRESS_H
#define FIBER_DNS_DNS_ADDRESS_H

#include <cstdint>
#include <type_traits>

#include "../net/IpAddress.h"

namespace fiber::dns {

inline constexpr std::uint16_t kDnsMaxAddressesPerFamily = 16;
inline constexpr std::uint16_t kDnsMaxAddressCount = kDnsMaxAddressesPerFamily * 2;

// Addresses are stored as one IPv4 segment followed by one IPv6 segment.
struct DnsAddressSet {
    net::IpAddress records[kDnsMaxAddressCount];
    std::uint16_t count;
    std::uint16_t v4_count;
};

static_assert(std::is_trivially_default_constructible_v<DnsAddressSet>);
static_assert(std::is_trivially_copyable_v<DnsAddressSet>);
static_assert(std::is_trivially_destructible_v<DnsAddressSet>);

} // namespace fiber::dns

#endif // FIBER_DNS_DNS_ADDRESS_H
