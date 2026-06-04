#ifndef FIBER_QUIC_QUIC_ACK_HANDLER_H
#define FIBER_QUIC_QUIC_ACK_HANDLER_H

#include <cstdint>

#include "../common/IoError.h"
#include "QuicConnection.h"
#include "QuicFrame.h"

namespace fiber::quic {

struct QuicAckProcessResult {
    bool unblocked = false;
    bool lost_frames = false;
    bool acked_frames = false;
};

[[nodiscard]] common::IoResult<QuicAckProcessResult>
quic_handle_ack_frame(QuicConnection &connection, QuicEncryptionLevel level, const QuicFrame &frame, QuicTime now,
                      std::uint64_t ack_delay_exponent = 3, QuicTime max_ack_delay = QuicTime{25},
                      bool handshake_confirmed = false) noexcept;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_ACK_HANDLER_H
