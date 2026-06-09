#ifndef FIBER_QUIC_QUIC_PACKET_CODEC_H
#define FIBER_QUIC_QUIC_PACKET_CODEC_H

#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "QuicConnection.h"
#include "QuicCursor.h"
#include "QuicProtocol.h"

namespace fiber::quic {

struct QuicPacketEncodeSpec {
    QuicEncryptionLevel level = QuicEncryptionLevel::Initial;
    QuicConnectionId dcid{};
    QuicConnectionId scid{};
    QuicSlice token{};
    QuicOutputFrameQueue *frame_queue = nullptr;
    QuicOutputFrame *frames = nullptr;
    std::size_t frame_count = 0;
    std::size_t min_packet_len = 0;
    std::size_t max_packet_len = 0;
};

struct QuicPacketEncodeResult {
    std::size_t packet_len = 0;
    std::uint64_t packet_number = 0;
    std::uint32_t frame_count = 0;
    bool ack_eliciting = false;
};

struct QuicPacketDecodeResult {
    QuicPacketHeader header{};
    QuicSlice payload{};
    std::uint32_t frame_count = 0;
    bool ack_eliciting = false;
};

struct QuicRetryPacketSpec {
    std::uint32_t version = kQuicVersion1;
    QuicConnectionId original_dcid{};
    QuicConnectionId dcid{};
    QuicConnectionId scid{};
    QuicSlice token{};
};

[[nodiscard]] common::IoResult<std::size_t> quic_create_version_negotiation_packet(const QuicPacketHeader &request,
                                                                                   QuicWriteCursor &out) noexcept;

[[nodiscard]] common::IoResult<std::size_t> quic_create_retry_packet(const QuicRetryPacketSpec &spec,
                                                                     QuicWriteCursor &out) noexcept;

[[nodiscard]] common::IoResult<QuicConnectionId>
quic_get_packet_dcid(const std::uint8_t *datagram, std::size_t datagram_len, std::uint8_t short_dcid_len) noexcept;

[[nodiscard]] common::IoResult<QuicPacketEncodeResult> quic_encode_packet(QuicConnection &connection,
                                                                          const QuicPacketEncodeSpec &spec,
                                                                          std::uint8_t *out,
                                                                          std::size_t out_cap) noexcept;

[[nodiscard]] common::IoResult<QuicPacketDecodeResult>
quic_decode_packet(QuicConnection &connection, std::uint8_t *datagram, std::size_t datagram_len,
                   std::uint8_t short_dcid_len, std::uint8_t *plaintext, std::size_t plaintext_cap) noexcept;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_PACKET_CODEC_H
