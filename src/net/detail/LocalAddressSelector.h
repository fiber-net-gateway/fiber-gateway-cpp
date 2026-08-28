#ifndef FIBER_NET_DETAIL_LOCAL_ADDRESS_SELECTOR_H
#define FIBER_NET_DETAIL_LOCAL_ADDRESS_SELECTOR_H

#include <cstdint>

#include <fiber/net/IpAddress.h>

namespace fiber::net::detail {

struct LocalIpv4Candidate {
    IpAddress address;
    std::uint32_t interface_index = 0;
    bool interface_up = false;
    bool interface_loopback = false;
};

[[nodiscard]] bool is_usable_local_ipv4_candidate(const LocalIpv4Candidate &candidate) noexcept;

[[nodiscard]] bool local_ipv4_candidate_precedes(const LocalIpv4Candidate &candidate,
                                                 const LocalIpv4Candidate &current) noexcept;

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_LOCAL_ADDRESS_SELECTOR_H
