#ifndef FIBER_ACCESS_SERVER_ACCESS_CONFIG_ERROR_H
#define FIBER_ACCESS_SERVER_ACCESS_CONFIG_ERROR_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace fiber::access_server {

enum class AccessConfigErrorCode : std::uint8_t {
    InvalidJson,
    InvalidRoot,
    InvalidField,
    OutOfRange,
    InvalidCombination,
    Conflict,
};

struct AccessConfigError {
    AccessConfigErrorCode code = AccessConfigErrorCode::InvalidJson;
    std::size_t offset = 0;
    std::string field;
    std::string message;

    bool operator==(const AccessConfigError &) const = default;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_CONFIG_ERROR_H
