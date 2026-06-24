#ifndef FIBER_QUIC_QUIC_CONNECTION_ID_H
#define FIBER_QUIC_QUIC_CONNECTION_ID_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "../common/IntrusiveRbTree.h"
#include "../common/IoError.h"
#include "QuicFrame.h"

namespace fiber::quic {

class QuicConnection;

inline constexpr std::size_t kQuicConnectionIdLength = kMaxConnectionIdLength;
inline constexpr std::size_t kQuicLocalConnectionIdSlotCount = 3;

struct QuicConnectionId {
    std::array<std::uint8_t, kMaxConnectionIdLength> bytes{};
    std::uint8_t length = 0;

    [[nodiscard]] bool empty() const noexcept { return length == 0; }
    [[nodiscard]] const std::uint8_t *data() const noexcept { return bytes.data(); }
    [[nodiscard]] std::size_t size() const noexcept { return length; }

    static common::IoResult<QuicConnectionId> from_bytes(const std::uint8_t *data, std::size_t len) noexcept;
};

struct QuicConnectionIdIndex {
    QuicConnection *connection = nullptr;
    QuicConnectionId cid_key{};
    std::uint64_t cid_hash = 0;
    common::IntrusiveRbTreeHook cid_hook{};
};

struct QuicLocalConnectionIdSlot {
    QuicConnectionIdIndex endpoint_index{};
    std::uint64_t sequence_number = 0;
    bool used = false;
    bool advertised = false;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_CONNECTION_ID_H
