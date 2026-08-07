#ifndef FIBER_ACCESS_SERVER_CIDR_H
#define FIBER_ACCESS_SERVER_CIDR_H

#include <fiber/net/IpAddress.h>
#include "../config/AccessConfigError.h"

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace fiber::access_server {

class Cidr {
public:
    [[nodiscard]] static std::expected<Cidr, AccessConfigError> parse(std::string_view text, std::string_view field);
    [[nodiscard]] static std::expected<std::vector<Cidr>, AccessConfigError>
    parse_list(std::span<const std::string_view> values, std::string_view field);

    [[nodiscard]] bool contains(const Cidr &other) const noexcept;
    [[nodiscard]] bool matches(const Cidr &other) const noexcept;
    [[nodiscard]] bool matches(const net::IpAddress &address) const noexcept;
    [[nodiscard]] std::uint8_t prefix_length() const noexcept { return prefix_length_; }
    [[nodiscard]] bool is_v4() const noexcept { return byte_size_ == net::IpAddress::kV4Size; }

private:
    std::array<std::uint8_t, net::IpAddress::kV6Size> network_{};
    std::uint8_t byte_size_ = 0;
    std::uint8_t prefix_length_ = 0;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_CIDR_H
