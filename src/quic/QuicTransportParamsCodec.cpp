#include "QuicTransportParamsCodec.h"

#include <cstring>
#include <expected>

#include "QuicTransportCodec.h"

namespace fiber::quic {

namespace {

[[nodiscard]] bool server_only_transport_param(std::uint64_t id) noexcept {
    return id == kQuicTpOriginalDestinationConnectionId || id == kQuicTpPreferredAddress ||
           id == kQuicTpStatelessResetToken || id == kQuicTpRetrySourceConnectionId;
}

[[nodiscard]] bool constrained_transport_params_valid(const QuicTransportParams &params) noexcept {
    return params.max_udp_payload_size >= kMinInitialDatagramSize &&
           params.max_udp_payload_size <= kQuicMaxUdpPayloadSize && params.ack_delay_exponent <= 20 &&
           params.max_ack_delay < (1ULL << 14U) && params.active_connection_id_limit >= 2;
}

[[nodiscard]] common::IoResult<std::uint64_t> parse_exact_varint(QuicReadCursor &in) noexcept {
    auto value = quic_parse_varint(in);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!in.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return *value;
}

[[nodiscard]] common::IoResult<QuicConnectionId> parse_connection_id_param(QuicReadCursor &in) noexcept {
    auto slice = in.read_slice(in.remaining());
    if (!slice) {
        return std::unexpected(slice.error());
    }
    auto cid = QuicConnectionId::from_bytes(slice->data, slice->len);
    if (!cid) {
        return std::unexpected(cid.error());
    }
    return *cid;
}

[[nodiscard]] std::size_t param_header_len(std::uint64_t id, std::size_t value_len) noexcept {
    return quic_varint_len(id) + quic_varint_len(value_len);
}

[[nodiscard]] common::IoResult<void> write_or_count_varint(QuicWriteCursor *out, std::uint64_t value,
                                                           std::size_t &len) noexcept {
    len += quic_varint_len(value);
    if (out == nullptr) {
        return {};
    }
    return quic_write_varint(*out, value);
}

[[nodiscard]] common::IoResult<void> write_or_count_bytes(QuicWriteCursor *out, const std::uint8_t *data,
                                                          std::size_t bytes, std::size_t &len) noexcept {
    len += bytes;
    if (out == nullptr) {
        return {};
    }
    return out->write_bytes(data, bytes);
}

[[nodiscard]] common::IoResult<void> write_varint_param(QuicWriteCursor *out, std::uint64_t id, std::uint64_t value,
                                                        std::size_t &len) noexcept {
    len += param_header_len(id, quic_varint_len(value)) + quic_varint_len(value);
    if (out == nullptr) {
        return {};
    }
    auto wrote = quic_write_varint(*out, id);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = quic_write_varint(*out, quic_varint_len(value));
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    return quic_write_varint(*out, value);
}

[[nodiscard]] common::IoResult<void> write_empty_param(QuicWriteCursor *out, std::uint64_t id,
                                                       std::size_t &len) noexcept {
    len += param_header_len(id, 0);
    if (out == nullptr) {
        return {};
    }
    auto wrote = quic_write_varint(*out, id);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    return quic_write_varint(*out, 0);
}

[[nodiscard]] common::IoResult<void> write_bytes_param(QuicWriteCursor *out, std::uint64_t id, const std::uint8_t *data,
                                                       std::size_t bytes, std::size_t &len) noexcept {
    auto wrote = write_or_count_varint(out, id, len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = write_or_count_varint(out, bytes, len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    return write_or_count_bytes(out, data, bytes, len);
}

} // namespace

bool quic_is_reserved_transport_param(std::uint64_t id) noexcept { return id % 31U == 27U; }

common::IoResult<void> quic_parse_transport_params(QuicTransportParamOwner owner, QuicReadCursor &in,
                                                   QuicTransportParams &out) noexcept {
    out = QuicTransportParams{};
    std::uint32_t seen_params = 0;

    while (!in.empty()) {
        auto id = quic_parse_varint(in);
        if (!id) {
            return std::unexpected(id.error());
        }
        auto len = quic_parse_varint(in);
        if (!len) {
            return std::unexpected(len.error());
        }
        auto value = in.read_slice(static_cast<std::size_t>(*len));
        if (!value) {
            return std::unexpected(value.error());
        }

        if (*id <= kQuicTpRetrySourceConnectionId) {
            const std::uint32_t param_bit = std::uint32_t{1} << static_cast<std::uint32_t>(*id);
            if ((seen_params & param_bit) != 0) {
                return std::unexpected(common::IoErr::Invalid);
            }
            seen_params |= param_bit;
        }

        if (owner == QuicTransportParamOwner::Client && server_only_transport_param(*id)) {
            return std::unexpected(common::IoErr::Invalid);
        }

        QuicReadCursor param(value->data, value->len);
        switch (*id) {
            case kQuicTpDisableActiveMigration:
                if (!param.empty()) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                out.disable_active_migration = true;
                break;

            case kQuicTpMaxIdleTimeout: {
                auto parsed = parse_exact_varint(param);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                out.max_idle_timeout = *parsed;
                break;
            }
            case kQuicTpMaxUdpPayloadSize: {
                auto parsed = parse_exact_varint(param);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                out.max_udp_payload_size = *parsed;
                break;
            }
            case kQuicTpInitialMaxData: {
                auto parsed = parse_exact_varint(param);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                out.initial_max_data = *parsed;
                break;
            }
            case kQuicTpInitialMaxStreamDataBidiLocal: {
                auto parsed = parse_exact_varint(param);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                out.initial_max_stream_data_bidi_local = *parsed;
                break;
            }
            case kQuicTpInitialMaxStreamDataBidiRemote: {
                auto parsed = parse_exact_varint(param);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                out.initial_max_stream_data_bidi_remote = *parsed;
                break;
            }
            case kQuicTpInitialMaxStreamDataUni: {
                auto parsed = parse_exact_varint(param);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                out.initial_max_stream_data_uni = *parsed;
                break;
            }
            case kQuicTpInitialMaxStreamsBidi: {
                auto parsed = parse_exact_varint(param);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                if (*parsed > kQuicMaxStreamLimit) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                out.initial_max_streams_bidi = *parsed;
                break;
            }
            case kQuicTpInitialMaxStreamsUni: {
                auto parsed = parse_exact_varint(param);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                if (*parsed > kQuicMaxStreamLimit) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                out.initial_max_streams_uni = *parsed;
                break;
            }
            case kQuicTpAckDelayExponent: {
                auto parsed = parse_exact_varint(param);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                out.ack_delay_exponent = *parsed;
                break;
            }
            case kQuicTpMaxAckDelay: {
                auto parsed = parse_exact_varint(param);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                out.max_ack_delay = *parsed;
                break;
            }
            case kQuicTpActiveConnectionIdLimit: {
                auto parsed = parse_exact_varint(param);
                if (!parsed) {
                    return std::unexpected(parsed.error());
                }
                out.active_connection_id_limit = *parsed;
                break;
            }
            case kQuicTpInitialSourceConnectionId: {
                auto cid = parse_connection_id_param(param);
                if (!cid) {
                    return std::unexpected(cid.error());
                }
                out.initial_source_connection_id = *cid;
                out.has_initial_source_connection_id = true;
                break;
            }
            case kQuicTpOriginalDestinationConnectionId: {
                auto cid = parse_connection_id_param(param);
                if (!cid) {
                    return std::unexpected(cid.error());
                }
                out.original_destination_connection_id = *cid;
                out.has_original_destination_connection_id = true;
                break;
            }
            case kQuicTpRetrySourceConnectionId: {
                auto cid = parse_connection_id_param(param);
                if (!cid) {
                    return std::unexpected(cid.error());
                }
                out.retry_source_connection_id = *cid;
                out.has_retry_source_connection_id = true;
                break;
            }
            case kQuicTpStatelessResetToken:
                if (param.remaining() != kStatelessResetTokenLength) {
                    return std::unexpected(common::IoErr::Invalid);
                }
                {
                    auto copied = param.copy_bytes(out.stateless_reset_token, kStatelessResetTokenLength);
                    if (!copied) {
                        return std::unexpected(copied.error());
                    }
                }
                out.has_stateless_reset_token = true;
                break;

            default:
                break;
        }
    }

    if (!constrained_transport_params_valid(out)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

common::IoResult<std::size_t> quic_create_transport_params(QuicTransportParamOwner owner, QuicWriteCursor *out,
                                                           const QuicTransportParams &params,
                                                           std::size_t *zero_rtt_len) noexcept {
    if (owner == QuicTransportParamOwner::Client &&
        (params.has_original_destination_connection_id || params.has_retry_source_connection_id ||
         params.has_stateless_reset_token)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!constrained_transport_params_valid(params) || params.initial_max_streams_bidi > kQuicMaxStreamLimit ||
        params.initial_max_streams_uni > kQuicMaxStreamLimit) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t len = 0;
    auto wrote = write_varint_param(out, kQuicTpInitialMaxData, params.initial_max_data, len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = write_varint_param(out, kQuicTpInitialMaxStreamsUni, params.initial_max_streams_uni, len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = write_varint_param(out, kQuicTpInitialMaxStreamsBidi, params.initial_max_streams_bidi, len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = write_varint_param(out, kQuicTpInitialMaxStreamDataBidiLocal, params.initial_max_stream_data_bidi_local,
                               len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = write_varint_param(out, kQuicTpInitialMaxStreamDataBidiRemote, params.initial_max_stream_data_bidi_remote,
                               len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = write_varint_param(out, kQuicTpInitialMaxStreamDataUni, params.initial_max_stream_data_uni, len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = write_varint_param(out, kQuicTpMaxIdleTimeout, params.max_idle_timeout, len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = write_varint_param(out, kQuicTpMaxUdpPayloadSize, params.max_udp_payload_size, len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    if (params.disable_active_migration) {
        wrote = write_empty_param(out, kQuicTpDisableActiveMigration, len);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
    }
    wrote = write_varint_param(out, kQuicTpActiveConnectionIdLimit, params.active_connection_id_limit, len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }

    if (zero_rtt_len != nullptr) {
        *zero_rtt_len = len;
    }

    wrote = write_varint_param(out, kQuicTpMaxAckDelay, params.max_ack_delay, len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = write_varint_param(out, kQuicTpAckDelayExponent, params.ack_delay_exponent, len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }

    if (params.has_initial_source_connection_id) {
        wrote = write_bytes_param(out, kQuicTpInitialSourceConnectionId, params.initial_source_connection_id.data(),
                                  params.initial_source_connection_id.size(), len);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
    }
    if (owner == QuicTransportParamOwner::Server && params.has_original_destination_connection_id) {
        wrote = write_bytes_param(out, kQuicTpOriginalDestinationConnectionId,
                                  params.original_destination_connection_id.data(),
                                  params.original_destination_connection_id.size(), len);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
    }
    if (owner == QuicTransportParamOwner::Server && params.has_retry_source_connection_id) {
        wrote = write_bytes_param(out, kQuicTpRetrySourceConnectionId, params.retry_source_connection_id.data(),
                                  params.retry_source_connection_id.size(), len);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
    }
    if (owner == QuicTransportParamOwner::Server && params.has_stateless_reset_token) {
        wrote = write_bytes_param(out, kQuicTpStatelessResetToken, params.stateless_reset_token,
                                  kStatelessResetTokenLength, len);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
    }

    return len;
}

} // namespace fiber::quic
