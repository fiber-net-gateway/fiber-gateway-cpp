#ifndef FIBER_COMMON_IO_ERROR_H
#define FIBER_COMMON_IO_ERROR_H

#include <cstdint>
#include <expected>
#include <string_view>

namespace fiber::common {

enum class IoErr : std::uint16_t {
    None = 0,
    WouldBlock,
    Interrupted,
    Invalid,
    BadFd,
    Busy,
    NotFound,
    AddrInUse,
    AddrNotAvailable,
    ConnAborted,
    ConnReset,
    ConnRefused,
    TimedOut,
    NotConnected,
    Already,
    Permission,
    BrokenPipe,
    NoMem,
    NotSupported,
    Canceled,
    Unknown,
};

template <typename E>
using IoResult = std::expected<E, IoErr>;

IoErr io_err_from_errno(int err) noexcept;
int io_err_to_errno(IoErr err) noexcept;
std::string_view io_err_name(IoErr err) noexcept;

} // namespace fiber::common

#endif // FIBER_COMMON_IO_ERROR_H
