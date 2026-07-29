#include "LocalAddress.h"

#include "SocketAddress.h"
#include "detail/LocalAddressSelector.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

namespace fiber::net {
namespace detail {

bool is_usable_local_ipv4_candidate(const LocalIpv4Candidate &candidate) noexcept {
    if (!candidate.interface_up || candidate.interface_loopback || candidate.interface_index == 0 ||
        !candidate.address.is_v4() || candidate.address.is_unspecified() || candidate.address.is_loopback() ||
        candidate.address.is_multicast()) {
        return false;
    }
    const auto bytes = candidate.address.v4_bytes();
    return bytes[0] != 169 || bytes[1] != 254;
}

bool local_ipv4_candidate_precedes(const LocalIpv4Candidate &candidate, const LocalIpv4Candidate &current) noexcept {
    if (candidate.interface_index != current.interface_index) {
        return candidate.interface_index < current.interface_index;
    }
    return candidate.address.v4_bytes() < current.address.v4_bytes();
}

} // namespace detail

std::expected<LocalIpv4Selection, LocalIpv4Error> detect_local_ipv4() noexcept {
    ifaddrs *interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) {
        return std::unexpected(LocalIpv4Error{
                .code = LocalIpv4ErrorCode::QueryFailed,
                .system_error = errno,
        });
    }

    bool found = false;
    detail::LocalIpv4Candidate best_candidate{};
    LocalIpv4Selection best{};
    for (const ifaddrs *item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || !item->ifa_name || item->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        SocketAddress socket_address;
        if (!SocketAddress::from_sockaddr(item->ifa_addr, sizeof(sockaddr_in), socket_address)) {
            continue;
        }
        detail::LocalIpv4Candidate candidate{
                .address = socket_address.ip(),
                .interface_index = ::if_nametoindex(item->ifa_name),
                .interface_up = (item->ifa_flags & IFF_UP) != 0,
                .interface_loopback = (item->ifa_flags & IFF_LOOPBACK) != 0,
        };
        if (!detail::is_usable_local_ipv4_candidate(candidate) ||
            (found && !detail::local_ipv4_candidate_precedes(candidate, best_candidate))) {
            continue;
        }

        found = true;
        best_candidate = candidate;
        best.address = candidate.address;
        best.interface_index = candidate.interface_index;
        best.interface_name.fill('\0');
        const std::string_view name(item->ifa_name);
        const std::size_t name_size = std::min(name.size(), best.interface_name.size() - 1);
        std::memcpy(best.interface_name.data(), name.data(), name_size);
        best.interface_name_size = static_cast<std::uint8_t>(name_size);
    }
    ::freeifaddrs(interfaces);

    if (!found) {
        return std::unexpected(LocalIpv4Error{
                .code = LocalIpv4ErrorCode::NotFound,
        });
    }
    return best;
}

} // namespace fiber::net
