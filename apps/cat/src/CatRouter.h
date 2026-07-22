#ifndef FIBER_CAT_ROUTER_H
#define FIBER_CAT_ROUTER_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

#include <net/SocketAddress.h>

namespace fiber::cat::detail {

enum class RouterParseError : std::uint8_t {
    InvalidJson,
    InvalidResponse,
    InvalidCollector,
    TooManyCollectors,
    NoMemory,
};

struct RouterSnapshot {
    std::vector<net::SocketAddress> collectors;
    double sample = 1.0;
    bool block = false;
};

[[nodiscard]] std::expected<RouterSnapshot, RouterParseError>
parse_router_response(std::string_view json, std::size_t max_collectors) noexcept;

} // namespace fiber::cat::detail

#endif // FIBER_CAT_ROUTER_H
