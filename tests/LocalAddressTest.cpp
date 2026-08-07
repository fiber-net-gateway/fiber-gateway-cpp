#include <gtest/gtest.h>

#include <fiber/net/detail/LocalAddressSelector.h>

namespace {

using fiber::net::IpAddress;
using fiber::net::detail::is_usable_local_ipv4_candidate;
using fiber::net::detail::local_ipv4_candidate_precedes;
using fiber::net::detail::LocalIpv4Candidate;

LocalIpv4Candidate candidate(IpAddress address, std::uint32_t interface_index = 2) {
    return {
            .address = address,
            .interface_index = interface_index,
            .interface_up = true,
    };
}

TEST(LocalAddressTest, RejectsUnusableInterfaceAddresses) {
    auto usable = candidate(IpAddress::v4({10, 0, 0, 8}));
    EXPECT_TRUE(is_usable_local_ipv4_candidate(usable));

    auto down = usable;
    down.interface_up = false;
    EXPECT_FALSE(is_usable_local_ipv4_candidate(down));

    auto loopback_interface = usable;
    loopback_interface.interface_loopback = true;
    EXPECT_FALSE(is_usable_local_ipv4_candidate(loopback_interface));

    auto missing_index = usable;
    missing_index.interface_index = 0;
    EXPECT_FALSE(is_usable_local_ipv4_candidate(missing_index));

    EXPECT_FALSE(is_usable_local_ipv4_candidate(candidate(IpAddress::any_v4())));
    EXPECT_FALSE(is_usable_local_ipv4_candidate(candidate(IpAddress::loopback_v4())));
    EXPECT_FALSE(is_usable_local_ipv4_candidate(candidate(IpAddress::v4({239, 1, 2, 3}))));
    EXPECT_FALSE(is_usable_local_ipv4_candidate(candidate(IpAddress::v4({169, 254, 2, 3}))));
    EXPECT_FALSE(is_usable_local_ipv4_candidate(candidate(IpAddress::loopback_v6())));
}

TEST(LocalAddressTest, SelectsDeterministicallyByInterfaceIndexThenAddress) {
    const auto lower_index = candidate(IpAddress::v4({192, 168, 1, 20}), 2);
    const auto higher_index = candidate(IpAddress::v4({10, 0, 0, 8}), 7);
    EXPECT_TRUE(local_ipv4_candidate_precedes(lower_index, higher_index));
    EXPECT_FALSE(local_ipv4_candidate_precedes(higher_index, lower_index));

    const auto lower_address = candidate(IpAddress::v4({10, 0, 0, 8}), 2);
    const auto higher_address = candidate(IpAddress::v4({10, 0, 0, 9}), 2);
    EXPECT_TRUE(local_ipv4_candidate_precedes(lower_address, higher_address));
    EXPECT_FALSE(local_ipv4_candidate_precedes(higher_address, lower_address));
    EXPECT_FALSE(local_ipv4_candidate_precedes(lower_address, lower_address));
}

} // namespace
