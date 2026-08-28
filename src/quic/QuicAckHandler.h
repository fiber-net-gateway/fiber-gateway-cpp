#ifndef FIBER_QUIC_QUIC_ACK_HANDLER_H
#define FIBER_QUIC_QUIC_ACK_HANDLER_H

#include <cstdint>

#include <fiber/common/IoError.h>
#include <fiber/quic/QuicConnection.h>
#include <fiber/quic/QuicFrame.h>

namespace fiber::quic {

struct QuicAckProcessResult {
    bool unblocked = false;
    bool lost_frames = false;
    bool acked_frames = false;
    bool force_send = false; // set when a lost ACK frame requires immediate re-generation
};

[[nodiscard]] common::IoResult<QuicAckProcessResult>
quic_handle_ack_frame(QuicConnection &connection, QuicEncryptionLevel level, const QuicInputFrame &frame, QuicTime now,
                      std::uint64_t ack_delay_exponent = 3, QuicTime max_ack_delay = QuicTime{25},
                      bool handshake_confirmed = false) noexcept;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_ACK_HANDLER_H
