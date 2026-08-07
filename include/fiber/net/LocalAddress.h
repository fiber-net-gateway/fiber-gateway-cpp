#ifndef FIBER_NET_LOCAL_ADDRESS_H
#define FIBER_NET_LOCAL_ADDRESS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

#include "IpAddress.h"

namespace fiber::net {

enum class LocalIpv4ErrorCode : std::uint8_t {
    QueryFailed,
    NotFound,
};

struct LocalIpv4Error {
    LocalIpv4ErrorCode code = LocalIpv4ErrorCode::NotFound;
    int system_error = 0;
};

struct LocalIpv4Selection {
    static constexpr std::size_t kInterfaceNameCapacity = 64;

    IpAddress address;
    std::uint32_t interface_index = 0;
    std::array<char, kInterfaceNameCapacity> interface_name{};
    std::uint8_t interface_name_size = 0;

    [[nodiscard]] std::string_view interface_name_view() const noexcept {
        return std::string_view(interface_name.data(), interface_name_size);
    }
};

[[nodiscard]] std::expected<LocalIpv4Selection, LocalIpv4Error> detect_local_ipv4() noexcept;

} // namespace fiber::net

#endif // FIBER_NET_LOCAL_ADDRESS_H
