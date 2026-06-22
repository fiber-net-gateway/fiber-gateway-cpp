#ifndef FIBER_QUIC_QUIC_CONNECTION_ID_H
#define FIBER_QUIC_QUIC_CONNECTION_ID_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "QuicFrame.h"

namespace fiber::quic {

inline constexpr std::size_t kQuicConnectionIdLength = kMaxConnectionIdLength;

struct QuicConnectionId {
    std::array<std::uint8_t, kMaxConnectionIdLength> bytes{};
    std::uint8_t length = 0;

    [[nodiscard]] bool empty() const noexcept { return length == 0; }
    [[nodiscard]] const std::uint8_t *data() const noexcept { return bytes.data(); }
    [[nodiscard]] std::size_t size() const noexcept { return length; }

    static common::IoResult<QuicConnectionId> from_bytes(const std::uint8_t *data, std::size_t len) noexcept;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_CONNECTION_ID_H
