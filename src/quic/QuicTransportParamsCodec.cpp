#include <fiber/quic/QuicTransportParamsCodec.h>

#include <cstring>
#include <expected>

#include <fiber/quic/QuicTransportCodec.h>

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

[[nodiscard]] common::IoResult<QuicPreferredAddress> parse_preferred_address(QuicReadCursor &in) noexcept {
    constexpr std::size_t kFixedLength = 4 + 2 + 16 + 2 + 1 + kStatelessResetTokenLength;
    if (in.remaining() < kFixedLength) {
        return std::unexpected(common::IoErr::Invalid);
    }
    std::array<std::uint8_t, net::IpAddress::kV4Size> ipv4{};
    std::array<std::uint8_t, net::IpAddress::kV6Size> ipv6{};
    auto copied = in.copy_bytes(ipv4.data(), ipv4.size());
    auto ipv4_port = copied ? in.read_be16() : std::unexpected(copied.error());
    copied = ipv4_port ? in.copy_bytes(ipv6.data(), ipv6.size()) : std::unexpected(ipv4_port.error());
    auto ipv6_port = copied ? in.read_be16() : std::unexpected(copied.error());
    auto cid_len = ipv6_port ? in.read_u8() : std::unexpected(ipv6_port.error());
    if (!cid_len || *cid_len == 0 || *cid_len > kMaxConnectionIdLength ||
        in.remaining() != static_cast<std::size_t>(*cid_len) + kStatelessResetTokenLength) {
        return std::unexpected(cid_len ? common::IoErr::Invalid : cid_len.error());
    }
    auto cid_bytes = in.read_slice(*cid_len);
    auto cid = cid_bytes ? QuicConnectionId::from_bytes(cid_bytes->data, cid_bytes->len)
                         : std::unexpected(cid_bytes.error());
    if (!cid) {
        return std::unexpected(cid.error());
    }
    QuicPreferredAddress result{};
    result.ipv4 = {net::IpAddress::v4(ipv4), *ipv4_port};
    result.ipv6 = {net::IpAddress::v6(ipv6), *ipv6_port};
    result.connection_id = *cid;
    copied = in.copy_bytes(result.stateless_reset_token, kStatelessResetTokenLength);
    if (!copied || !in.empty()) {
        return std::unexpected(copied ? common::IoErr::Invalid : copied.error());
    }
    return result;
}

[[nodiscard]] common::IoResult<void>
write_preferred_address(QuicWriteCursor *out, const QuicPreferredAddress &preferred, std::size_t &len) noexcept {
    if (preferred.connection_id.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    constexpr std::size_t kFixedLength = 4 + 2 + 16 + 2 + 1 + kStatelessResetTokenLength;
    const std::size_t value_len = kFixedLength + preferred.connection_id.size();
    len += quic_varint_len(kQuicTpPreferredAddress) + quic_varint_len(value_len) + value_len;
    if (out == nullptr) {
        return {};
    }
    auto wrote = quic_write_varint(*out, kQuicTpPreferredAddress);
    if (wrote) {
        wrote = quic_write_varint(*out, value_len);
    }
    if (wrote) {
        const auto ipv4 =
                preferred.ipv4.ip().is_v4() ? preferred.ipv4.ip().v4_bytes() : net::IpAddress::any_v4().v4_bytes();
        wrote = out->write_bytes(ipv4.data(), ipv4.size());
    }
    if (wrote) {
        wrote = out->write_be16(preferred.ipv4.ip().is_v4() ? preferred.ipv4.port() : 0);
    }
    if (wrote) {
        const auto ipv6 =
                preferred.ipv6.ip().is_v6() ? preferred.ipv6.ip().v6_bytes() : net::IpAddress::any_v6().v6_bytes();
        wrote = out->write_bytes(ipv6.data(), ipv6.size());
    }
    if (wrote) {
        wrote = out->write_be16(preferred.ipv6.ip().is_v6() ? preferred.ipv6.port() : 0);
    }
    if (wrote) {
        wrote = out->write_u8(preferred.connection_id.length);
    }
    if (wrote) {
        wrote = out->write_bytes(preferred.connection_id.data(), preferred.connection_id.size());
    }
    if (wrote) {
        wrote = out->write_bytes(preferred.stateless_reset_token, kStatelessResetTokenLength);
    }
    return wrote;
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

            case kQuicTpPreferredAddress: {
                auto preferred = parse_preferred_address(param);
                if (!preferred) {
                    return std::unexpected(preferred.error());
                }
                out.preferred_address = *preferred;
                out.has_preferred_address = true;
                break;
            }

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
         params.has_stateless_reset_token || params.has_preferred_address)) {
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
    if (owner == QuicTransportParamOwner::Server && params.has_preferred_address) {
        wrote = write_preferred_address(out, params.preferred_address, len);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
    }

    return len;
}

} // namespace fiber::quic
