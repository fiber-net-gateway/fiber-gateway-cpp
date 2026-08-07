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
// Capacity for peer-issued CIDs we track per connection (RFC 9000 §5.1.1).
// Sized larger than the advertised active_connection_id_limit so that a peer
// NEW_CONNECTION_ID frame which adds one CID and only later retires others
// can be processed without temporarily exceeding the slot array (cf. nginx
// NGX_QUIC_MAX_SERVER_IDS = 8). We advertise 4 and reserve 8 slots.
inline constexpr std::size_t kQuicRemoteConnectionIdSlotCount = 8;

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

struct QuicStatelessResetTokenIndex {
    QuicConnection *connection = nullptr;
    const std::uint8_t *token = nullptr;
    QuicStatelessResetTokenIndex *next = nullptr;
    std::uint64_t token_hash = 0;
    bool linked = false;
};

// Peer-issued Connection ID held in the connection's remote-CID pool. Created
// either at handshake time from the peer's initial Source Connection ID
// (sequence_number = 0) or in response to a NEW_CONNECTION_ID frame.
//
// in_use marks the slot as populated with a valid CID still active in the pool.
// used marks the slot as bound to a QuicPath; per RFC 9000 §9.5 a CID MUST NOT
// be sent to more than one local address, so once used the slot stays sticky
// until the peer retires it via retire_prior_to (whereupon the slot is wiped
// and becomes available for a future NEW_CONNECTION_ID).
struct QuicRemoteConnectionIdSlot {
    QuicConnectionId cid{};
    std::uint64_t sequence_number = 0;
    std::uint8_t stateless_reset_token[kStatelessResetTokenLength]{};
    QuicStatelessResetTokenIndex reset_token_index{};
    bool in_use = false;
    bool used = false;
    bool has_stateless_reset_token = false;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_CONNECTION_ID_H
