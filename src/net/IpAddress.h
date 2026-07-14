#ifndef FIBER_NET_IP_ADDRESS_H
#define FIBER_NET_IP_ADDRESS_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include "../common/Assert.h"

namespace fiber::net {

enum class IpFamily : std::uint8_t { V4, V6 };

class IpAddress {
public:
    static constexpr std::size_t kV4Size = 4;
    static constexpr std::size_t kV6Size = 16;

    IpAddress() = default;

    static IpAddress v4(std::array<std::uint8_t, kV4Size> bytes) noexcept;
    static IpAddress v6(std::array<std::uint8_t, kV6Size> bytes, std::uint32_t scope_id = 0) noexcept;

    static IpAddress any_v4() noexcept;
    static IpAddress any_v6() noexcept;
    static IpAddress loopback_v4() noexcept;
    static IpAddress loopback_v6() noexcept;

    [[nodiscard]] IpFamily family() const noexcept { return family_; }
    [[nodiscard]] bool is_v4() const noexcept { return family_ == IpFamily::V4; }
    [[nodiscard]] bool is_v6() const noexcept { return family_ == IpFamily::V6; }
    [[nodiscard]] std::uint32_t scope_id() const noexcept { return scope_id_; }
    [[nodiscard]] bool is_loopback() const noexcept;
    [[nodiscard]] bool is_unspecified() const noexcept;
    [[nodiscard]] bool is_multicast() const noexcept;
    [[nodiscard]] std::array<std::uint8_t, kV4Size> v4_bytes() const noexcept {
        FIBER_ASSERT(is_v4());
        return {bytes_[0], bytes_[1], bytes_[2], bytes_[3]};
    }
    [[nodiscard]] const std::array<std::uint8_t, kV6Size> &v6_bytes() const noexcept {
        FIBER_ASSERT(is_v6());
        return bytes_;
    }
    [[nodiscard]] const std::uint8_t *data() const noexcept { return bytes_.data(); }
    [[nodiscard]] std::size_t byte_size() const noexcept { return is_v4() ? kV4Size : kV6Size; }

    static bool parse(std::string_view text, IpAddress &out) noexcept;
    std::string to_string() const;

    friend bool operator==(const IpAddress &left, const IpAddress &right) noexcept = default;

private:
    std::array<std::uint8_t, kV6Size> bytes_;
    std::uint32_t scope_id_;
    IpFamily family_;
};

static_assert(std::is_trivially_default_constructible_v<IpAddress>);
static_assert(std::is_trivially_copyable_v<IpAddress>);
static_assert(std::is_trivially_destructible_v<IpAddress>);
static_assert(std::is_standard_layout_v<IpAddress>);

} // namespace fiber::net

#endif // FIBER_NET_IP_ADDRESS_H
