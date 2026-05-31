#ifndef FIBER_QUIC_QUIC_TRANSPORT_CODEC_H
#define FIBER_QUIC_QUIC_TRANSPORT_CODEC_H

#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "QuicCursor.h"
#include "QuicProtocol.h"

namespace fiber::quic {

[[nodiscard]] common::IoResult<std::uint64_t> quic_parse_varint(QuicReadCursor &in) noexcept;
[[nodiscard]] common::IoResult<void> quic_write_varint(QuicWriteCursor &out, std::uint64_t value) noexcept;
[[nodiscard]] std::size_t quic_varint_len(std::uint64_t value) noexcept;

[[nodiscard]] common::IoResult<QuicPacketHeader>
quic_parse_packet_header(const std::uint8_t *datagram, std::size_t datagram_len, std::uint8_t short_dcid_len) noexcept;

[[nodiscard]] common::IoResult<std::size_t> quic_create_packet_header(QuicWriteCursor &out,
                                                                      const QuicPacketHeader &packet,
                                                                      std::uint8_t **packet_number_pos) noexcept;

[[nodiscard]] std::size_t quic_packet_payload_capacity(const QuicPacketHeader &packet,
                                                       std::size_t target_packet_len) noexcept;

[[nodiscard]] bool quic_frame_allowed(QuicEncryptionLevel level, QuicFrameType frame_type) noexcept;
[[nodiscard]] common::IoResult<QuicFrameParseResult> quic_parse_frame(QuicEncryptionLevel level,
                                                                      QuicReadCursor &payload) noexcept;
[[nodiscard]] common::IoResult<std::size_t> quic_create_frame(QuicWriteCursor *out, QuicFrame &frame) noexcept;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_TRANSPORT_CODEC_H
