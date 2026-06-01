#ifndef FIBER_QUIC_QUIC_TRANSPORT_PARAMS_CODEC_H
#define FIBER_QUIC_QUIC_TRANSPORT_PARAMS_CODEC_H

#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "QuicCursor.h"
#include "QuicProtocol.h"

namespace fiber::quic {

inline constexpr std::uint64_t kQuicTpOriginalDestinationConnectionId = 0x00;
inline constexpr std::uint64_t kQuicTpMaxIdleTimeout = 0x01;
inline constexpr std::uint64_t kQuicTpStatelessResetToken = 0x02;
inline constexpr std::uint64_t kQuicTpMaxUdpPayloadSize = 0x03;
inline constexpr std::uint64_t kQuicTpInitialMaxData = 0x04;
inline constexpr std::uint64_t kQuicTpInitialMaxStreamDataBidiLocal = 0x05;
inline constexpr std::uint64_t kQuicTpInitialMaxStreamDataBidiRemote = 0x06;
inline constexpr std::uint64_t kQuicTpInitialMaxStreamDataUni = 0x07;
inline constexpr std::uint64_t kQuicTpInitialMaxStreamsBidi = 0x08;
inline constexpr std::uint64_t kQuicTpInitialMaxStreamsUni = 0x09;
inline constexpr std::uint64_t kQuicTpAckDelayExponent = 0x0A;
inline constexpr std::uint64_t kQuicTpMaxAckDelay = 0x0B;
inline constexpr std::uint64_t kQuicTpDisableActiveMigration = 0x0C;
inline constexpr std::uint64_t kQuicTpPreferredAddress = 0x0D;
inline constexpr std::uint64_t kQuicTpActiveConnectionIdLimit = 0x0E;
inline constexpr std::uint64_t kQuicTpInitialSourceConnectionId = 0x0F;
inline constexpr std::uint64_t kQuicTpRetrySourceConnectionId = 0x10;

enum class QuicTransportParamOwner : std::uint8_t {
    Client,
    Server,
};

struct QuicTransportParams {
    std::uint64_t max_idle_timeout = 0;
    std::uint64_t max_udp_payload_size = 65527;
    std::uint64_t initial_max_data = 0;
    std::uint64_t initial_max_stream_data_bidi_local = 0;
    std::uint64_t initial_max_stream_data_bidi_remote = 0;
    std::uint64_t initial_max_stream_data_uni = 0;
    std::uint64_t initial_max_streams_bidi = 0;
    std::uint64_t initial_max_streams_uni = 0;
    std::uint64_t ack_delay_exponent = 3;
    std::uint64_t max_ack_delay = 25;
    std::uint64_t active_connection_id_limit = 2;

    bool disable_active_migration = false;
    bool has_original_destination_connection_id = false;
    bool has_initial_source_connection_id = false;
    bool has_retry_source_connection_id = false;
    bool has_stateless_reset_token = false;

    QuicConnectionId original_destination_connection_id{};
    QuicConnectionId initial_source_connection_id{};
    QuicConnectionId retry_source_connection_id{};
    std::uint8_t stateless_reset_token[kStatelessResetTokenLength]{};
};

[[nodiscard]] bool quic_is_reserved_transport_param(std::uint64_t id) noexcept;
[[nodiscard]] common::IoResult<void> quic_parse_transport_params(QuicTransportParamOwner owner, QuicReadCursor &in,
                                                                 QuicTransportParams &out) noexcept;
[[nodiscard]] common::IoResult<std::size_t> quic_create_transport_params(QuicTransportParamOwner owner,
                                                                         QuicWriteCursor *out,
                                                                         const QuicTransportParams &params,
                                                                         std::size_t *zero_rtt_len = nullptr) noexcept;

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_TRANSPORT_PARAMS_CODEC_H
