#include <fiber/http/Http3ControlStreamEncoder.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

#include <fiber/quic/QuicTransportCodec.h>

namespace fiber::http {

namespace {

[[nodiscard]] std::size_t setting_len(std::uint64_t id, std::uint64_t value) noexcept {
    return quic::quic_varint_len(id) + quic::quic_varint_len(value);
}

[[nodiscard]] std::size_t settings_payload_len(const Http3Settings &settings) noexcept {
    std::size_t len = 0;
    if (settings.qpack_max_table_capacity != 0) {
        len += setting_len(static_cast<std::uint64_t>(Http3SettingId::QpackMaxTableCapacity),
                           settings.qpack_max_table_capacity);
    }
    if (settings.max_field_section_size != 0) {
        len += setting_len(static_cast<std::uint64_t>(Http3SettingId::MaxFieldSectionSize),
                           settings.max_field_section_size);
    }
    if (settings.qpack_blocked_streams != 0) {
        len += setting_len(static_cast<std::uint64_t>(Http3SettingId::QpackBlockedStreams),
                           settings.qpack_blocked_streams);
    }
    if (settings.enable_connect_protocol) {
        len += setting_len(static_cast<std::uint64_t>(Http3SettingId::EnableConnectProtocol), 1);
    }
    return len;
}

[[nodiscard]] common::IoResult<void> write_varint(quic::QuicWriteCursor &out, std::uint64_t value) noexcept {
    return quic::quic_write_varint(out, value);
}

[[nodiscard]] common::IoResult<void> write_setting(quic::QuicWriteCursor &out, Http3SettingId id,
                                                   std::uint64_t value) noexcept {
    auto wrote_id = write_varint(out, static_cast<std::uint64_t>(id));
    if (!wrote_id) {
        return std::unexpected(wrote_id.error());
    }
    return write_varint(out, value);
}

} // namespace

common::IoResult<mem::IoBufChain> encode_http3_control_stream_preface(const Http3Settings &settings,
                                                                      mem::IoBufNodePool &node_pool) noexcept {
    const std::size_t payload_len = settings_payload_len(settings);
    const std::size_t encoded_len = quic::quic_varint_len(static_cast<std::uint64_t>(Http3StreamType::Control)) +
                                    quic::quic_varint_len(static_cast<std::uint64_t>(Http3FrameType::Settings)) +
                                    quic::quic_varint_len(payload_len) + payload_len;

    mem::IoBuf buf = mem::IoBuf::allocate(encoded_len);
    if (!buf) {
        return std::unexpected(common::IoErr::NoMem);
    }

    quic::QuicWriteCursor out(buf.writable_data(), buf.writable());
    auto wrote = write_varint(out, static_cast<std::uint64_t>(Http3StreamType::Control));
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = write_varint(out, static_cast<std::uint64_t>(Http3FrameType::Settings));
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = write_varint(out, payload_len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }

    if (settings.qpack_max_table_capacity != 0) {
        wrote = write_setting(out, Http3SettingId::QpackMaxTableCapacity, settings.qpack_max_table_capacity);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
    }
    if (settings.max_field_section_size != 0) {
        wrote = write_setting(out, Http3SettingId::MaxFieldSectionSize, settings.max_field_section_size);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
    }
    if (settings.qpack_blocked_streams != 0) {
        wrote = write_setting(out, Http3SettingId::QpackBlockedStreams, settings.qpack_blocked_streams);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
    }
    if (settings.enable_connect_protocol) {
        wrote = write_setting(out, Http3SettingId::EnableConnectProtocol, 1);
        if (!wrote) {
            return std::unexpected(wrote.error());
        }
    }

    buf.commit(out.offset());
    mem::IoBufChain chain(node_pool);
    if (!chain.append(std::move(buf))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return std::move(chain);
}

common::IoResult<mem::IoBufChain> encode_http3_goaway_frame(std::uint64_t id, mem::IoBufNodePool &node_pool) noexcept {
    const std::size_t payload_len = quic::quic_varint_len(id);
    const std::size_t encoded_len = quic::quic_varint_len(static_cast<std::uint64_t>(Http3FrameType::Goaway)) +
                                    quic::quic_varint_len(payload_len) + payload_len;
    mem::IoBuf buf = mem::IoBuf::allocate(encoded_len);
    if (!buf) {
        return std::unexpected(common::IoErr::NoMem);
    }

    quic::QuicWriteCursor out(buf.writable_data(), buf.writable());
    auto wrote = write_varint(out, static_cast<std::uint64_t>(Http3FrameType::Goaway));
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = write_varint(out, payload_len);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }
    wrote = write_varint(out, id);
    if (!wrote) {
        return std::unexpected(wrote.error());
    }

    buf.commit(out.offset());
    mem::IoBufChain chain(node_pool);
    if (!chain.append(std::move(buf))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return std::move(chain);
}

} // namespace fiber::http
